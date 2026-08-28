// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0

#include "brake_health/v1/window.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace brake_health::v1 {
namespace {

constexpr std::size_t maximum_pre_samples = 30U;
constexpr std::size_t maximum_active_samples = 100U;
constexpr std::size_t maximum_post_samples = 20U;
constexpr std::size_t maximum_total_samples = 150U;
constexpr std::int64_t maximum_pre_duration_ms = 3000;

}  // namespace

WindowEngine::WindowEngine(UuidSource uuid_source) : uuid_source_(std::move(uuid_source)) {
    if (!uuid_source_) {
        throw std::invalid_argument("UUID source is required");
    }
}

bool WindowEngine::qualifying_trigger(const SourceFrame& frame) const {
    return frame.speed_kph >= 10.0 && frame.brake_pedal_percent >= 50;
}

bool WindowEngine::qualifying_clear(const SourceFrame& frame) const {
    return frame.brake_pedal_percent < 10 || frame.speed_kph < 0.5;
}

bool WindowEngine::update_hold(
    bool condition,
    std::int64_t now_ms,
    std::int64_t duration_ms,
    std::optional<std::int64_t>& hold_start) {
    if (!condition) {
        hold_start.reset();
        return false;
    }
    if (!hold_start) {
        hold_start = now_ms;
    }
    return now_ms - *hold_start >= duration_ms;
}

void WindowEngine::prune_pre_samples(std::int64_t now_ms) {
    while (!pre_ring_.empty() &&
           now_ms - pre_ring_.front().frame.monotonic_ms > maximum_pre_duration_ms) {
        pre_ring_.pop_front();
    }
}

void WindowEngine::add_pre_sample(const SourceFrame& frame) {
    prune_pre_samples(frame.monotonic_ms);
    pre_ring_.push_back({frame, Phase::Pre});
    while (pre_ring_.size() > maximum_pre_samples) {
        pre_ring_.pop_front();
    }
}

void WindowEngine::add_capture_sample(const SourceFrame& frame, Phase phase) {
    if (samples_.size() >= maximum_total_samples) {
        return;
    }
    samples_.push_back({frame, phase});
}

void WindowEngine::begin_event(const SourceFrame& frame) {
    event_id_ = uuid_source_();
    if (event_id_.empty()) {
        throw std::runtime_error("UUID source returned an empty identifier");
    }
    prune_pre_samples(frame.monotonic_ms);
    trigger_timestamp_ = frame.source_timestamp;
    samples_.assign(pre_ring_.begin(), pre_ring_.end());
    pre_ring_.clear();
    state_ = State::Active;
    event_start_ms_ =
        samples_.empty() ? frame.monotonic_ms : samples_.front().frame.monotonic_ms;
    active_segment_start_ms_ = frame.monotonic_ms;
    accumulated_active_ms_ = 0;
    clear_hold_start_.reset();
    trigger_hold_start_.reset();
}

std::int64_t WindowEngine::active_elapsed_ms(std::int64_t now_ms) const {
    if (state_ == State::Active) {
        return accumulated_active_ms_ + (now_ms - active_segment_start_ms_);
    }
    return accumulated_active_ms_;
}

std::size_t WindowEngine::phase_count(Phase phase) const {
    return static_cast<std::size_t>(std::count_if(
        samples_.begin(), samples_.end(), [phase](const RetainedSample& sample) {
            return sample.phase == phase;
        }));
}

std::optional<EventWindow> WindowEngine::finish(TerminalState state, ReasonCode reason) {
    if (state_ == State::Idle) {
        return std::nullopt;
    }
    EventWindow window{
        event_id_,
        trigger_timestamp_,
        state,
        reason,
        std::move(samples_),
    };
    state_ = State::Idle;
    event_id_.clear();
    trigger_timestamp_.clear();
    samples_.clear();
    trigger_hold_start_.reset();
    clear_hold_start_.reset();
    accumulated_active_ms_ = 0;
    if (state == TerminalState::TruncatedMaxDuration) {
        retrigger_suppressed_ = true;
        suppression_clear_hold_start_.reset();
    }
    return window;
}

IngestResult WindowEngine::ingest(const SourceFrame& frame) {
    IngestResult result;
    result.validation = validator_.validate(
        frame, previous_source_epoch_ms_, previous_monotonic_ms_);
    if (!result.validation.valid) {
        trigger_hold_start_.reset();
        clear_hold_start_.reset();
        suppression_clear_hold_start_.reset();
        if (state_ != State::Idle) {
            result.completed = finish(
                TerminalState::IncompleteSourceGap, ReasonCode::SourceGap);
        }
        result.event_active = state_ != State::Idle;
        return result;
    }

    previous_source_epoch_ms_ = frame.source_epoch_ms;
    previous_monotonic_ms_ = frame.monotonic_ms;
    ++valid_frame_count_;
    result.retained = valid_frame_count_ % 3U == 0U;

    if (retrigger_suppressed_) {
        if (update_hold(
                qualifying_clear(frame),
                frame.monotonic_ms,
                500,
                suppression_clear_hold_start_)) {
            retrigger_suppressed_ = false;
            suppression_clear_hold_start_.reset();
        }
        if (result.retained) {
            add_pre_sample(frame);
        }
        return result;
    }

    if (state_ == State::Idle) {
        const bool trigger = update_hold(
            qualifying_trigger(frame), frame.monotonic_ms, 200, trigger_hold_start_);
        if (trigger) {
            begin_event(frame);
            result.event_started = true;
            if (result.retained) {
                add_capture_sample(frame, Phase::Active);
            }
        } else if (result.retained) {
            add_pre_sample(frame);
        }
        result.event_active = state_ != State::Idle;
        return result;
    }

    if (frame.monotonic_ms - event_start_ms_ >= 15000 ||
        active_elapsed_ms(frame.monotonic_ms) >= 10000 ||
        samples_.size() >= maximum_total_samples ||
        phase_count(Phase::Active) >= maximum_active_samples) {
        result.completed = finish(
            TerminalState::TruncatedMaxDuration, ReasonCode::MaxActiveDuration);
        return result;
    }

    if (state_ == State::Active) {
        if (update_hold(
                qualifying_clear(frame), frame.monotonic_ms, 500, clear_hold_start_)) {
            accumulated_active_ms_ += frame.monotonic_ms - active_segment_start_ms_;
            post_start_ms_ = frame.monotonic_ms;
            state_ = State::Post;
            clear_hold_start_.reset();
            trigger_hold_start_.reset();
        }
        if (result.retained) {
            add_capture_sample(frame, state_ == State::Post ? Phase::Post : Phase::Active);
        }
    } else {
        const bool retrigger = update_hold(
            qualifying_trigger(frame), frame.monotonic_ms, 200, trigger_hold_start_);
        if (retrigger) {
            state_ = State::Active;
            active_segment_start_ms_ = frame.monotonic_ms;
            trigger_hold_start_.reset();
            clear_hold_start_.reset();
            result.event_started = true;
        } else if (frame.monotonic_ms - post_start_ms_ >= 2000 ||
                   phase_count(Phase::Post) >= maximum_post_samples) {
            result.completed = finish(TerminalState::Complete, ReasonCode::NormalClear);
            return result;
        }
        if (result.retained) {
            add_capture_sample(frame, state_ == State::Active ? Phase::Active : Phase::Post);
        }
    }

    if (samples_.size() >= maximum_total_samples) {
        result.completed = finish(
            TerminalState::TruncatedMaxDuration, ReasonCode::MaxActiveDuration);
    }
    result.event_active = state_ != State::Idle;
    return result;
}

std::optional<EventWindow> WindowEngine::abort_service_stop() {
    return finish(TerminalState::AbortedServiceStop, ReasonCode::ServiceStop);
}

std::optional<EventWindow> WindowEngine::abort_restart() {
    return finish(TerminalState::AbortedRestart, ReasonCode::ServiceRestart);
}

std::size_t WindowEngine::pre_sample_count() const {
    return pre_ring_.size();
}

std::size_t WindowEngine::active_sample_count() const {
    return phase_count(Phase::Active);
}

std::size_t WindowEngine::total_sample_count() const {
    return samples_.size();
}

bool WindowEngine::capturing() const {
    return state_ != State::Idle;
}

bool WindowEngine::retrigger_suppressed() const {
    return retrigger_suppressed_;
}

const char* phase_name(Phase phase) {
    switch (phase) {
        case Phase::Pre:
            return "PRE";
        case Phase::Active:
            return "ACTIVE";
        case Phase::Post:
            return "POST";
    }
    throw std::invalid_argument("unknown phase");
}

const char* terminal_state_name(TerminalState state) {
    switch (state) {
        case TerminalState::Complete:
            return "COMPLETE";
        case TerminalState::TruncatedMaxDuration:
            return "TRUNCATED_MAX_DURATION";
        case TerminalState::IncompleteSourceGap:
            return "INCOMPLETE_SOURCE_GAP";
        case TerminalState::AbortedServiceStop:
            return "ABORTED_SERVICE_STOP";
        case TerminalState::AbortedRestart:
            return "ABORTED_RESTART";
    }
    throw std::invalid_argument("unknown terminal state");
}

const char* reason_code_name(ReasonCode reason) {
    switch (reason) {
        case ReasonCode::NormalClear:
            return "NORMAL_CLEAR";
        case ReasonCode::MaxActiveDuration:
            return "MAX_ACTIVE_DURATION";
        case ReasonCode::SourceGap:
            return "SOURCE_GAP";
        case ReasonCode::ServiceStop:
            return "SERVICE_STOP";
        case ReasonCode::ServiceRestart:
            return "SERVICE_RESTART";
    }
    throw std::invalid_argument("unknown reason code");
}

}  // namespace brake_health::v1
