// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace brake_health::v2 {

enum class TerminalState {
    Complete,
    TruncatedMaxDuration,
    IncompleteSourceGap,
    AbortedServiceStop,
    AbortedRestart,
};

enum class Phase { Pre, Active, Post };

struct FixedSignals {
    std::int64_t speed_milli_kph{};
    std::int64_t longitudinal_acceleration_milli_mps2{};
    std::int64_t brake_effort_milli_percent{};
    std::int64_t steering_milli_degree{};
    std::array<std::int64_t, 4> wheel_angular_milli_degree_per_second{};
    std::array<std::int64_t, 4> wheel_linear_milli_kph{};
};

struct Sample {
    std::uint32_t sample_index{};
    std::string source_timestamp;
    Phase phase{Phase::Pre};
    // Preserve rejected adapter evidence without permitting a floating-point
    // value into the deterministic model decision path.
    bool complete{true};
    bool finite{true};
    std::int32_t max_source_age_ms{};
    FixedSignals signals;
};

struct CompletedEpisode {
    std::string source_event_id;
    TerminalState terminal_state{TerminalState::Complete};
    std::uint32_t retained_cadence_hz{10};
    std::vector<Sample> samples;
};

enum class SkipReason {
    EpisodeNotComplete,
    MissingRequiredSignal,
    StaleSample,
    NonFiniteValue,
    OutOfRangeValue,
    NonMonotonicSourceTime,
    InsufficientActiveSamples,
    InsufficientQualifiedWheelSamples,
};

const char* skip_reason_name(SkipReason reason);

}  // namespace brake_health::v2
