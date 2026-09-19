// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "brake_health/v1/model.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace brake_health::v1 {

enum class Phase { Pre, Active, Post };

enum class TerminalState {
    Complete,
    TruncatedMaxDuration,
    IncompleteSourceGap,
    AbortedServiceStop,
    AbortedRestart,
};

enum class ReasonCode {
    NormalClear,
    MaxActiveDuration,
    SourceGap,
    ServiceStop,
    ServiceRestart,
};

struct RetainedSample {
    SourceFrame frame;
    Phase phase{Phase::Pre};
};

struct EventWindow {
    std::string event_id;
    std::string trigger_timestamp;
    TerminalState terminal_state{TerminalState::Complete};
    ReasonCode reason_code{ReasonCode::NormalClear};
    std::vector<RetainedSample> samples;
};

struct IngestResult {
    FrameValidation validation;
    bool retained{};
    bool event_started{};
    bool event_active{};
    std::optional<EventWindow> completed;
};

using UuidSource = std::function<std::string()>;

class WindowEngine {
public:
    explicit WindowEngine(UuidSource uuid_source);

    IngestResult ingest(const SourceFrame& frame);
    std::optional<EventWindow> abort_service_stop();
    std::optional<EventWindow> abort_restart();

    std::size_t pre_sample_count() const;
    std::size_t active_sample_count() const;
    std::size_t total_sample_count() const;
    bool capturing() const;
    const char* activity_state() const {return state_==State::Active?"ACTIVE":state_==State::Post?"POST":"WAITING";}
    std::optional<std::string> activity_id() const {return capturing()?std::optional<std::string>{event_id_}:std::nullopt;}
    bool retrigger_suppressed() const;

private:
    enum class State { Idle, Active, Post };

    bool qualifying_trigger(const SourceFrame& frame) const;
    bool qualifying_clear(const SourceFrame& frame) const;
    bool update_hold(
        bool condition,
        std::int64_t now_ms,
        std::int64_t duration_ms,
        std::optional<std::int64_t>& hold_start);
    void prune_pre_samples(std::int64_t now_ms);
    void add_pre_sample(const SourceFrame& frame);
    void add_capture_sample(const SourceFrame& frame, Phase phase);
    void begin_event(const SourceFrame& frame);
    std::optional<EventWindow> finish(TerminalState state, ReasonCode reason);
    std::int64_t active_elapsed_ms(std::int64_t now_ms) const;
    std::size_t phase_count(Phase phase) const;

    FrameValidator validator_;
    UuidSource uuid_source_;
    State state_{State::Idle};
    std::deque<RetainedSample> pre_ring_;
    std::vector<RetainedSample> samples_;
    std::string event_id_;
    std::string trigger_timestamp_;
    std::optional<std::int64_t> previous_source_epoch_ms_;
    std::optional<std::int64_t> previous_monotonic_ms_;
    std::optional<std::int64_t> trigger_hold_start_;
    std::optional<std::int64_t> clear_hold_start_;
    std::optional<std::int64_t> suppression_clear_hold_start_;
    std::int64_t event_start_ms_{};
    std::int64_t active_segment_start_ms_{};
    std::int64_t accumulated_active_ms_{};
    std::int64_t post_start_ms_{};
    std::optional<std::int64_t> retained_source_bucket_;
    bool retrigger_suppressed_{};
};

const char* phase_name(Phase phase);
const char* terminal_state_name(TerminalState state);
const char* reason_code_name(ReasonCode reason);

}  // namespace brake_health::v1
