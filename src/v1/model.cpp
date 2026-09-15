// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0

#include "brake_health/v1/model.hpp"

#include <cmath>
#include <string>

namespace brake_health::v1 {
namespace {

bool digits(const std::string& value, std::size_t offset, std::size_t count) {
    if (offset + count > value.size()) {
        return false;
    }
    for (std::size_t index = offset; index < offset + count; ++index) {
        if (value[index] < '0' || value[index] > '9') {
            return false;
        }
    }
    return true;
}

int decimal(const std::string& value, std::size_t offset, std::size_t count) {
    int result = 0;
    for (std::size_t index = offset; index < offset + count; ++index) {
        result = result * 10 + (value[index] - '0');
    }
    return result;
}

bool leap_year(int year) {
    return year % 400 == 0 || (year % 4 == 0 && year % 100 != 0);
}

}  // namespace

bool is_rfc3339_millisecond_utc(const std::string& value) {
    if (value.size() != 24 || value[4] != '-' || value[7] != '-' ||
        value[10] != 'T' || value[13] != ':' || value[16] != ':' ||
        value[19] != '.' || value[23] != 'Z') {
        return false;
    }
    if (!digits(value, 0, 4) || !digits(value, 5, 2) || !digits(value, 8, 2) ||
        !digits(value, 11, 2) || !digits(value, 14, 2) || !digits(value, 17, 2) ||
        !digits(value, 20, 3)) {
        return false;
    }

    const int year = decimal(value, 0, 4);
    const int month = decimal(value, 5, 2);
    const int day = decimal(value, 8, 2);
    const int hour = decimal(value, 11, 2);
    const int minute = decimal(value, 14, 2);
    const int second = decimal(value, 17, 2);
    if (year == 0 || month < 1 || month > 12 || hour > 23 || minute > 59 || second > 59) {
        return false;
    }
    static constexpr int days_per_month[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
    };
    int maximum_day = days_per_month[month - 1];
    if (month == 2 && leap_year(year)) {
        maximum_day = 29;
    }
    return day >= 1 && day <= maximum_day;
}

FrameValidation FrameValidator::validate(
    const SourceFrame& frame,
    std::optional<std::int64_t> previous_source_epoch_ms,
    std::optional<std::int64_t> previous_monotonic_ms) const {
    if (frame.quality == FrameQuality::Incomplete) {
        return {false, FrameError::Incomplete};
    }
    if (frame.quality == FrameQuality::Malformed ||
        !is_rfc3339_millisecond_utc(frame.source_timestamp)) {
        return {false, FrameError::Malformed};
    }
    if (!std::isfinite(frame.speed_kph) ||
        !std::isfinite(frame.longitudinal_acceleration_mps2) ||
        !std::isfinite(frame.lateral_acceleration_mps2) ||
        !std::isfinite(frame.vertical_acceleration_mps2)) {
        return {false, FrameError::NonFinite};
    }
    if (frame.speed_kph < 0.0 || frame.speed_kph > 1000.0 ||
        frame.longitudinal_acceleration_mps2 < -100.0 ||
        frame.longitudinal_acceleration_mps2 > 100.0 ||
        frame.lateral_acceleration_mps2 < -100.0 ||
        frame.lateral_acceleration_mps2 > 100.0 ||
        frame.vertical_acceleration_mps2 < -100.0 ||
        frame.vertical_acceleration_mps2 > 100.0 ||
        frame.accelerator_pedal_percent < 0 || frame.accelerator_pedal_percent > 100 ||
        frame.brake_pedal_percent < 0 || frame.brake_pedal_percent > 100) {
        return {false, FrameError::OutOfRange};
    }
    if (frame.max_source_age_ms < 0) {
        return {false, FrameError::Future};
    }
    if (frame.max_source_age_ms > kMaximumSourceAgeMs) {
        return {false, FrameError::Stale};
    }
    if ((previous_source_epoch_ms && frame.source_epoch_ms < *previous_source_epoch_ms) ||
        (previous_monotonic_ms && frame.monotonic_ms < *previous_monotonic_ms)) {
        return {false, FrameError::Reordered};
    }
    return {true, FrameError::None};
}

}  // namespace brake_health::v1
