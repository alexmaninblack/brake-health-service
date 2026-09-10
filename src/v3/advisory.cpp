// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0

#include "brake_health/v3/advisory.hpp"

#include "brake_health/v1/model.hpp"

#include <array>
#include <cstdio>
#include <limits>
#include <stdexcept>

namespace brake_health::v3 {
namespace {

int decimal(std::string_view value, std::size_t at, std::size_t size) {
    int result = 0;
    for (std::size_t index = 0U; index < size; ++index) {
        const char character = value.at(at + index);
        if (character < '0' || character > '9') {
            throw std::invalid_argument("timestamp has a non-decimal field");
        }
        result = result * 10 + character - '0';
    }
    return result;
}

std::int64_t days_from_civil(int year, unsigned month, unsigned day) noexcept {
    year -= month <= 2U;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
    const unsigned day_of_year =
        (153U * (month + (month > 2U ? static_cast<unsigned>(-3) : 9U)) + 2U) /
            5U +
        day - 1U;
    const unsigned day_of_era =
        year_of_era * 365U + year_of_era / 4U - year_of_era / 100U + day_of_year;
    return static_cast<std::int64_t>(era) * 146097 +
           static_cast<std::int64_t>(day_of_era) - 719468;
}

std::array<int, 3> civil_from_days(std::int64_t days) noexcept {
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned day_of_era = static_cast<unsigned>(days - era * 146097);
    const unsigned year_of_era =
        (day_of_era - day_of_era / 1460U + day_of_era / 36524U -
         day_of_era / 146096U) /
        365U;
    int year = static_cast<int>(year_of_era) + static_cast<int>(era) * 400;
    const unsigned day_of_year =
        day_of_era - (365U * year_of_era + year_of_era / 4U -
                      year_of_era / 100U);
    const unsigned month_prime = (5U * day_of_year + 2U) / 153U;
    const unsigned day = day_of_year - (153U * month_prime + 2U) / 5U + 1U;
    const unsigned month =
        month_prime + (month_prime < 10U ? 3U : static_cast<unsigned>(-9));
    year += month <= 2U;
    return {year, static_cast<int>(month), static_cast<int>(day)};
}

}  // namespace

const char* gateway_state_name(GatewayState state) {
    switch (state) {
        case GatewayState::Received: return "RECEIVED";
        case GatewayState::Applied: return "APPLIED";
        case GatewayState::Cleared: return "CLEARED";
        case GatewayState::Rejected: return "REJECTED";
        case GatewayState::Expired: return "EXPIRED";
        case GatewayState::Failed: return "FAILED";
    }
    throw std::invalid_argument("unknown Gateway state");
}

const char* gateway_reason_name(GatewayReason reason) {
    switch (reason) {
        case GatewayReason::None: return "NONE";
        case GatewayReason::UnauthorizedSource: return "UNAUTHORIZED_SOURCE";
        case GatewayReason::UnauthorizedPath: return "UNAUTHORIZED_PATH";
        case GatewayReason::InvalidSchema: return "INVALID_SCHEMA";
        case GatewayReason::InvalidValue: return "INVALID_VALUE";
        case GatewayReason::StaleRequest: return "STALE_REQUEST";
        case GatewayReason::ReplayDetected: return "REPLAY_DETECTED";
        case GatewayReason::SequenceRollback: return "SEQUENCE_ROLLBACK";
        case GatewayReason::RateLimited: return "RATE_LIMITED";
        case GatewayReason::QmPolicyDenied: return "QM_POLICY_DENIED";
        case GatewayReason::InternalError: return "INTERNAL_ERROR";
    }
    throw std::invalid_argument("unknown Gateway reason");
}

const char* active_recommendation_name(ActiveRecommendation value) {
    switch (value) {
        case ActiveRecommendation::None: return "NONE";
        case ActiveRecommendation::InspectionRecommended:
            return "INSPECTION_RECOMMENDED";
    }
    throw std::invalid_argument("unknown active recommendation");
}

const char* active_reason_name(ActiveReason value) {
    switch (value) {
        case ActiveReason::None: return "NONE";
        case ActiveReason::PredictedBrakeDegradation:
            return "PREDICTED_BRAKE_DEGRADATION";
    }
    throw std::invalid_argument("unknown active reason");
}

bool application_evidence(GatewayState state) noexcept {
    return state == GatewayState::Applied || state == GatewayState::Cleared;
}

bool canonical_uuid(std::string_view value, char version) noexcept {
    if (value.size() != 36U || value[8] != '-' || value[13] != '-' ||
        value[18] != '-' || value[23] != '-' ||
        (version != '\0' && value[14] != version) ||
        std::string_view("89ab").find(value[19]) == std::string_view::npos) {
        return false;
    }
    for (std::size_t index = 0U; index < value.size(); ++index) {
        if (index == 8U || index == 13U || index == 18U || index == 23U) {
            continue;
        }
        const char character = value[index];
        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return true;
}

bool canonical_sha256(std::string_view value) noexcept {
    if (value.size() != 64U) return false;
    for (const char character : value) {
        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return true;
}

bool bounded_identifier(std::string_view value) noexcept {
    if (value.empty() || value.size() > 128U) return false;
    for (const char character : value) {
        if (!((character >= 'A' && character <= 'Z') ||
              (character >= 'a' && character <= 'z') ||
              (character >= '0' && character <= '9') || character == '.' ||
              character == '_' || character == ':' || character == '-')) {
            return false;
        }
    }
    return true;
}

bool semantic_version(std::string_view value) noexcept {
    if (value.empty() || value.size() > 32) return false;
    unsigned dots = 0U;
    bool digit = false;
    bool leading_zero = false;
    for (const char character : value) {
        if (character >= '0' && character <= '9') {
            if (digit && leading_zero) return false;
            if (!digit) leading_zero = character == '0';
            digit = true;
        } else if (character == '.' && digit && dots < 2U) {
            ++dots;
            digit = false;
        } else {
            return false;
        }
    }
    return dots == 2U && digit;
}

std::int64_t timestamp_milliseconds(const std::string& value) {
    if (!brake_health::v1::is_rfc3339_millisecond_utc(value)) {
        throw std::invalid_argument("timestamp must be canonical RFC3339 UTC milliseconds");
    }
    const int year = decimal(value, 0U, 4U);
    const unsigned month = static_cast<unsigned>(decimal(value, 5U, 2U));
    const unsigned day = static_cast<unsigned>(decimal(value, 8U, 2U));
    const int hour = decimal(value, 11U, 2U);
    const int minute = decimal(value, 14U, 2U);
    const int second = decimal(value, 17U, 2U);
    const int milliseconds = decimal(value, 20U, 3U);
    const std::int64_t days = days_from_civil(year, month, day);
    return (((days * 24 + hour) * 60 + minute) * 60 + second) * 1000 +
           milliseconds;
}

std::string timestamp_from_milliseconds(std::int64_t value) {
    std::int64_t days = value / 86400000;
    std::int64_t remainder = value % 86400000;
    if (remainder < 0) {
        remainder += 86400000;
        --days;
    }
    const auto civil = civil_from_days(days);
    const int hour = static_cast<int>(remainder / 3600000);
    remainder %= 3600000;
    const int minute = static_cast<int>(remainder / 60000);
    remainder %= 60000;
    const int second = static_cast<int>(remainder / 1000);
    const int millisecond = static_cast<int>(remainder % 1000);
    char encoded[25]{};
    const int count = std::snprintf(
        encoded, sizeof(encoded), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
        civil[0], civil[1], civil[2], hour, minute, second, millisecond);
    if (count != 24 || civil[0] < 0 || civil[0] > 9999) {
        throw std::out_of_range("timestamp is outside the supported year range");
    }
    return encoded;
}

}  // namespace brake_health::v3
