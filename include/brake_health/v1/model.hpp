// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace brake_health::v1 {

enum class FrameQuality {
    ValidCompleteFrame,
    Incomplete,
    Malformed,
};

enum class FrameError {
    None,
    Incomplete,
    Malformed,
    NonFinite,
    OutOfRange,
    Future,
    Stale,
    Reordered,
};

struct SourceFrame {
    double speed_kph{};
    double longitudinal_acceleration_mps2{};
    double lateral_acceleration_mps2{};
    double vertical_acceleration_mps2{};
    int accelerator_pedal_percent{};
    int brake_pedal_percent{};
    std::string source_timestamp;
    std::int64_t source_epoch_ms{};
    std::int64_t monotonic_ms{};
    int max_source_age_ms{};
    FrameQuality quality{FrameQuality::ValidCompleteFrame};
};

struct FrameValidation {
    bool valid{};
    FrameError error{FrameError::None};
};

class FrameValidator {
public:
    FrameValidation validate(
        const SourceFrame& frame,
        std::optional<std::int64_t> previous_source_epoch_ms,
        std::optional<std::int64_t> previous_monotonic_ms) const;
};

bool is_rfc3339_millisecond_utc(const std::string& value);

}  // namespace brake_health::v1
