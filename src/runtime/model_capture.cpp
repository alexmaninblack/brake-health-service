// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0
#include "brake_health/runtime/model_capture.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace brake_health::runtime {
std::int64_t quantize_milli(double value, double minimum, double maximum) {
    if (!std::isfinite(value) || value < minimum || value > maximum) throw std::invalid_argument("MODEL_INPUT_INVALID");
    // VAL float is promoted exactly to double; its 24-bit significand times
    // 1000 fits the double significand without a second model quantization.
    const double rounded = std::round(value * 1000.0);
    if (rounded < static_cast<long double>(std::numeric_limits<std::int64_t>::min()) ||
        rounded > static_cast<long double>(std::numeric_limits<std::int64_t>::max())) throw std::out_of_range("MODEL_INPUT_OVERFLOW");
    return static_cast<std::int64_t>(rounded);
}
std::optional<ModelFrame> complete_model_frame(const std::array<Signal, 12>& values,
    std::int64_t wall, std::int64_t mono, std::int64_t previous) {
    const auto epoch = values.front().epoch_ms;
    if (epoch <= previous || epoch < 0 || epoch > wall || wall - epoch > 250 || mono < 0) return {};
    for (const auto& value : values) if (!value.valid || value.epoch_ms != epoch) return {};
    try {
        ModelFrame result; result.source_epoch_ms = epoch; result.monotonic_ms = mono;
        result.source_age_ms = static_cast<std::int32_t>(wall - epoch);
        auto& s = result.signals;
        s.speed_milli_kph = quantize_milli(values[0].value, 0, 1000);
        s.longitudinal_acceleration_milli_mps2 = quantize_milli(values[1].value, -100, 100);
        s.brake_effort_milli_percent = quantize_milli(values[2].value, 0, 100);
        s.steering_milli_degree = quantize_milli(values[3].value, -360, 360);
        for (std::size_t i = 0; i < 4; ++i) {
            s.wheel_angular_milli_degree_per_second[i] = quantize_milli(values[i + 4].value, -10000, 10000);
            s.wheel_linear_milli_kph[i] = quantize_milli(values[i + 8].value, 0, 1000);
        }
        return result;
    } catch (const std::invalid_argument&) { return {}; }
}
ModelCapture::ModelCapture(v1::UuidSource uuid) : uuid_(std::move(uuid)) {
    if (!uuid_) throw std::invalid_argument("UUID_SOURCE_REQUIRED");
}
bool ModelCapture::hold(bool condition, std::int64_t now, std::int64_t duration, std::optional<std::int64_t>& start) {
    if (!condition) { start.reset(); return false; }
    if (!start) start = now;
    return now - *start >= duration;
}
void ModelCapture::prune(std::int64_t now) {
    while (!pre_.empty() && (now - pre_.front().monotonic_ms > 3000 || pre_.size() > 30)) pre_.pop_front();
}
void ModelCapture::retain(const ModelFrame& f, v2::Phase phase) {
    v2::Sample sample; sample.source_timestamp = utc_timestamp(f.source_epoch_ms); sample.phase = phase;
    sample.max_source_age_ms = f.source_age_ms; sample.signals = f.signals;
    if (phase == v2::Phase::Pre) { pre_.push_back({sample, f.monotonic_ms}); prune(f.monotonic_ms); }
    else if (episode_.samples.size() < 150) {
        sample.sample_index = static_cast<std::uint32_t>(episode_.samples.size()); episode_.samples.push_back(sample);
    }
}
std::size_t ModelCapture::count(v2::Phase p) const {
    return static_cast<std::size_t>(std::count_if(episode_.samples.begin(), episode_.samples.end(), [p](const auto& s) { return s.phase == p; }));
}
std::optional<v2::CompletedEpisode> ModelCapture::abort(v2::TerminalState terminal) {
    trigger_hold_.reset(); clear_hold_.reset(); suppression_hold_.reset();
    if (state_ == State::Idle) { pre_.clear(); return {}; }
    episode_.terminal_state = terminal;
    auto result = std::move(episode_); episode_ = {}; state_ = State::Idle; pre_.clear();
    if (terminal == v2::TerminalState::TruncatedMaxDuration) suppressed_ = true;
    return result;
}
std::optional<v2::CompletedEpisode> ModelCapture::ingest(const ModelFrame& f) {
    if ((previous_source_ && (f.source_epoch_ms <= *previous_source_ || f.source_epoch_ms - *previous_source_ > 250)) ||
        (previous_mono_ && (f.monotonic_ms < *previous_mono_ || f.monotonic_ms - *previous_mono_ > 250)) ||
        f.source_age_ms < 0 || f.source_age_ms > 250) {
        previous_source_ = f.source_epoch_ms; previous_mono_ = f.monotonic_ms;
        return abort(v2::TerminalState::IncompleteSourceGap);
    }
    previous_source_ = f.source_epoch_ms; previous_mono_ = f.monotonic_ms;
    const bool retained = ++frames_ % 3 == 0;
    const bool trigger = f.signals.speed_milli_kph >= 10000 && f.signals.brake_effort_milli_percent >= 50000;
    const bool clear = f.signals.brake_effort_milli_percent < 10000 || f.signals.speed_milli_kph < 500;
    if (suppressed_) {
        if (hold(clear, f.monotonic_ms, 500, suppression_hold_)) { suppressed_ = false; suppression_hold_.reset(); }
        if (retained) retain(f, v2::Phase::Pre);
        return {};
    }
    if (state_ == State::Idle) {
        if (hold(trigger, f.monotonic_ms, 200, trigger_hold_)) {
            prune(f.monotonic_ms); episode_ = {}; episode_.source_event_id = uuid_();
            for (auto& value : pre_) { value.sample.sample_index = static_cast<std::uint32_t>(episode_.samples.size()); episode_.samples.push_back(value.sample); }
            event_start_ = pre_.empty() ? f.monotonic_ms : pre_.front().monotonic_ms;
            pre_.clear(); state_ = State::Active; active_start_ = f.monotonic_ms; active_elapsed_ = 0;
            trigger_hold_.reset(); clear_hold_.reset();
            if (retained) retain(f, v2::Phase::Active);
        } else if (retained) retain(f, v2::Phase::Pre);
        return {};
    }
    const auto active_ms = active_elapsed_ + (state_ == State::Active ? f.monotonic_ms - active_start_ : 0);
    if (f.monotonic_ms - event_start_ >= 15000 || active_ms >= 10000 || episode_.samples.size() >= 150 || count(v2::Phase::Active) >= 100)
        return abort(v2::TerminalState::TruncatedMaxDuration);
    if (state_ == State::Active) {
        if (hold(clear, f.monotonic_ms, 500, clear_hold_)) {
            active_elapsed_ += f.monotonic_ms - active_start_; post_start_ = f.monotonic_ms; state_ = State::Post;
            clear_hold_.reset(); trigger_hold_.reset();
        }
    } else {
        if (hold(trigger, f.monotonic_ms, 200, trigger_hold_)) {
            state_ = State::Active; active_start_ = f.monotonic_ms; trigger_hold_.reset(); clear_hold_.reset();
        } else if (f.monotonic_ms - post_start_ >= 2000 || count(v2::Phase::Post) >= 20) return abort(v2::TerminalState::Complete);
    }
    if (retained) retain(f, state_ == State::Active ? v2::Phase::Active : v2::Phase::Post);
    if (episode_.samples.size() >= 150) return abort(v2::TerminalState::TruncatedMaxDuration);
    return {};
}
}  // namespace brake_health::runtime
