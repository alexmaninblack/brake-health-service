// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0

#include "brake_health/v2/model.hpp"

#include "brake_health/v1/model.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <set>
#include <stdexcept>

namespace brake_health::v2 {
namespace {

constexpr std::uint32_t clamp_bps(std::uint64_t value) {
    return static_cast<std::uint32_t>(value > 10000U ? 10000U : value);
}

bool valid_uuid4(const std::string& value) {
    if (value.size() != 36U || value[8] != '-' || value[13] != '-' ||
        value[18] != '-' || value[23] != '-' || value[14] != '4' ||
        std::string("89ab").find(value[19]) == std::string::npos) {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
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

bool signal_range_valid(const FixedSignals& signals) {
    const auto in_range = [](std::int64_t value, std::int64_t low, std::int64_t high) {
        return value >= low && value <= high;
    };
    if (!in_range(signals.speed_milli_kph, 0, 1000000) ||
        !in_range(signals.longitudinal_acceleration_milli_mps2, -100000, 100000) ||
        !in_range(signals.brake_effort_milli_percent, 0, 100000) ||
        !in_range(signals.steering_milli_degree, -360000, 360000)) {
        return false;
    }
    for (std::int64_t value : signals.wheel_linear_milli_kph) {
        if (!in_range(value, 0, 1000000)) {
            return false;
        }
    }
    for (std::int64_t value : signals.wheel_angular_milli_degree_per_second) {
        if (!in_range(value, -10000000, 10000000)) {
            return false;
        }
    }
    return true;
}

int phase_rank(Phase phase) {
    switch (phase) {
        case Phase::Pre:
            return 0;
        case Phase::Active:
            return 1;
        case Phase::Post:
            return 2;
    }
    return -1;
}

Evaluation skipped(const ModelState& state, SkipReason reason) {
    Evaluation result;
    result.skip_reason = reason;
    result.next_state = state;
    return result;
}

}  // namespace

const char* skip_reason_name(SkipReason reason) {
    switch (reason) {
        case SkipReason::EpisodeNotComplete:
            return "EPISODE_NOT_COMPLETE";
        case SkipReason::MissingRequiredSignal:
            return "MISSING_REQUIRED_SIGNAL";
        case SkipReason::StaleSample:
            return "STALE_SAMPLE";
        case SkipReason::NonFiniteValue:
            return "NON_FINITE_VALUE";
        case SkipReason::OutOfRangeValue:
            return "OUT_OF_RANGE_VALUE";
        case SkipReason::NonMonotonicSourceTime:
            return "NON_MONOTONIC_SOURCE_TIME";
        case SkipReason::InsufficientActiveSamples:
            return "INSUFFICIENT_ACTIVE_SAMPLES";
        case SkipReason::InsufficientQualifiedWheelSamples:
            return "INSUFFICIENT_QUALIFIED_WHEEL_SAMPLES";
    }
    return "UNKNOWN";
}

const char* condition_band_name(ConditionBand band) {
    switch (band) {
        case ConditionBand::Good:
            return "GOOD";
        case ConditionBand::Monitor:
            return "MONITOR";
        case ConditionBand::InspectionRecommended:
            return "INSPECTION_RECOMMENDED";
    }
    return "MONITOR";
}

std::uint64_t round_half_up(std::uint64_t numerator, std::uint64_t denominator) {
    if (denominator == 0U || numerator > std::numeric_limits<std::uint64_t>::max() - denominator / 2U) {
        throw std::overflow_error("invalid round-half-up operands");
    }
    return (numerator + denominator / 2U) / denominator;
}

ConditionBand band_for_score(std::uint32_t score) {
    if (score >= 70U) {
        return ConditionBand::Good;
    }
    if (score >= 40U) {
        return ConditionBand::Monitor;
    }
    return ConditionBand::InspectionRecommended;
}

ModelState initial_state(std::string producer_epoch) {
    if (!valid_uuid4(producer_epoch)) {
        throw std::invalid_argument("producer epoch must be a lowercase UUIDv4");
    }
    ModelState state;
    state.producer_epoch = std::move(producer_epoch);
    return state;
}

Evaluation SyntheticModel::evaluate(
    const CompletedEpisode& episode, const ModelState& current) const {
    const std::set<std::string> unique_recent(
        current.recent_source_event_ids.begin(), current.recent_source_event_ids.end());
    if (current.generation == std::numeric_limits<std::uint64_t>::max() ||
        current.wear_index > 100U || current.condition_score != 100U - current.wear_index ||
        current.condition_band != band_for_score(current.condition_score) ||
        !valid_uuid4(current.producer_epoch) || current.next_advisory_sequence == 0U ||
        current.recent_source_event_ids.size() > 64U ||
        unique_recent.size() != current.recent_source_event_ids.size() ||
        current.last_applied_source_event_id.has_value() !=
            current.last_assessment_id.has_value()) {
        throw std::invalid_argument("current model state is not eligible for evaluation");
    }
    if (episode.terminal_state != TerminalState::Complete) {
        return skipped(current, SkipReason::EpisodeNotComplete);
    }
    if (!valid_uuid4(episode.source_event_id) || episode.retained_cadence_hz != 10U ||
        episode.samples.empty() || episode.samples.size() > 150U) {
        return skipped(current, SkipReason::MissingRequiredSignal);
    }

    for (const Sample& sample : episode.samples) {
        if (!sample.complete) {
            return skipped(current, SkipReason::MissingRequiredSignal);
        }
    }
    bool has_pre = false;
    bool has_active = false;
    bool has_post = false;
    std::size_t active_phase_count = 0U;
    bool left_pre = false;
    for (const Sample& sample : episode.samples) {
        const int current_phase = phase_rank(sample.phase);
        // D4-016.1 permits POST -> ACTIVE retrigger under the same event ID.
        // PRE remains a prefix and POST cannot precede the first ACTIVE.
        if (current_phase < 0 || (sample.phase == Phase::Pre && left_pre) ||
            (sample.phase == Phase::Post && !has_active)) {
            return skipped(current, SkipReason::MissingRequiredSignal);
        }
        left_pre = left_pre || sample.phase != Phase::Pre;
        has_pre = has_pre || sample.phase == Phase::Pre;
        has_active = has_active || sample.phase == Phase::Active;
        has_post = has_post || sample.phase == Phase::Post;
        active_phase_count += sample.phase == Phase::Active ? 1U : 0U;
    }
    if (!has_pre || !has_active || !has_post || active_phase_count > 100U) {
        return skipped(current, SkipReason::MissingRequiredSignal);
    }
    for (const Sample& sample : episode.samples) {
        if (sample.max_source_age_ms < 0 || sample.max_source_age_ms > kMaximumSourceAgeMs) {
            return skipped(current, SkipReason::StaleSample);
        }
    }
    for (const Sample& sample : episode.samples) {
        if (!sample.finite) {
            return skipped(current, SkipReason::NonFiniteValue);
        }
    }
    for (const Sample& sample : episode.samples) {
        if (!signal_range_valid(sample.signals)) {
            return skipped(current, SkipReason::OutOfRangeValue);
        }
    }
    for (std::size_t index = 0; index < episode.samples.size(); ++index) {
        const Sample& sample = episode.samples[index];
        if (!brake_health::v1::is_rfc3339_millisecond_utc(sample.source_timestamp) ||
            sample.sample_index != index ||
            (index != 0U && sample.source_timestamp <= episode.samples[index - 1U].source_timestamp)) {
            return skipped(current, SkipReason::NonMonotonicSourceTime);
        }
    }

    std::vector<const Sample*> active;
    std::vector<const Sample*> straight;
    for (const Sample& sample : episode.samples) {
        if (sample.phase != Phase::Active) {
            continue;
        }
        active.push_back(&sample);
        if (sample.signals.speed_milli_kph >= 10000 &&
            std::llabs(sample.signals.steering_milli_degree) <= 5000 &&
            sample.max_source_age_ms <= kMaximumSourceAgeMs) {
            straight.push_back(&sample);
        }
    }
    if (active.size() < 5U) {
        return skipped(current, SkipReason::InsufficientActiveSamples);
    }
    if (straight.size() < 5U) {
        return skipped(current, SkipReason::InsufficientQualifiedWheelSamples);
    }

    FeatureVector features;
    features.active_sample_count = static_cast<std::uint32_t>(active.size());
    features.straight_active_sample_count = static_cast<std::uint32_t>(straight.size());
    std::uint64_t brake_sum = 0U;
    for (const Sample* sample : active) {
        features.peak_deceleration_milli_mps2 = std::max(
            features.peak_deceleration_milli_mps2,
            std::max<std::int64_t>(0, -sample->signals.longitudinal_acceleration_milli_mps2));
        brake_sum += static_cast<std::uint64_t>(sample->signals.brake_effort_milli_percent);
    }
    features.peak_deceleration_bps = clamp_bps(round_half_up(
        static_cast<std::uint64_t>(features.peak_deceleration_milli_mps2) * 10000U, 8000U));
    features.active_duration_milliseconds =
        static_cast<std::uint32_t>(active.size() * 100U);
    features.active_duration_bps = clamp_bps(round_half_up(
        static_cast<std::uint64_t>(features.active_duration_milliseconds) * 10000U, 5000U));
    features.speed_reduction_milli_kph = std::max<std::int64_t>(
        0, active.front()->signals.speed_milli_kph - active.back()->signals.speed_milli_kph);
    features.speed_reduction_bps = clamp_bps(round_half_up(
        static_cast<std::uint64_t>(features.speed_reduction_milli_kph) * 10000U, 40000U));
    features.mean_brake_milli_percent = static_cast<std::int64_t>(
        round_half_up(brake_sum, active.size()));
    const std::uint64_t brake_above = static_cast<std::uint64_t>(
        std::max<std::int64_t>(0, features.mean_brake_milli_percent - 50000));
    features.mean_brake_bps = clamp_bps(round_half_up(brake_above * 10000U, 50000U));

    for (const Sample* sample : straight) {
        const auto linear_minmax = std::minmax_element(
            sample->signals.wheel_linear_milli_kph.begin(),
            sample->signals.wheel_linear_milli_kph.end());
        const std::uint64_t linear_range = static_cast<std::uint64_t>(*linear_minmax.second - *linear_minmax.first);
        const std::uint64_t linear_denominator = static_cast<std::uint64_t>(
            std::max<std::int64_t>(*linear_minmax.second, 5000));
        const std::uint32_t linear = clamp_bps(round_half_up(
            linear_range * 10000U, linear_denominator));

        std::array<std::int64_t, 4> absolute_angular{};
        std::transform(
            sample->signals.wheel_angular_milli_degree_per_second.begin(),
            sample->signals.wheel_angular_milli_degree_per_second.end(),
            absolute_angular.begin(), [](std::int64_t value) { return std::llabs(value); });
        const auto angular_minmax = std::minmax_element(
            absolute_angular.begin(), absolute_angular.end());
        const std::uint64_t angular_range = static_cast<std::uint64_t>(
            *angular_minmax.second - *angular_minmax.first);
        const std::uint64_t angular_denominator = static_cast<std::uint64_t>(
            std::max<std::int64_t>(*angular_minmax.second, 30000));
        const std::uint32_t angular = clamp_bps(round_half_up(
            angular_range * 10000U, angular_denominator));
        features.wheel_dispersion_raw_bps = std::max(
            features.wheel_dispersion_raw_bps, std::max(linear, angular));
    }
    features.wheel_dispersion_bps = clamp_bps(round_half_up(
        static_cast<std::uint64_t>(features.wheel_dispersion_raw_bps) * 10000U, 1500U));
    const std::uint64_t weighted =
        30U * features.peak_deceleration_bps +
        20U * features.active_duration_bps +
        15U * features.speed_reduction_bps +
        15U * features.mean_brake_bps +
        20U * features.wheel_dispersion_bps;
    features.episode_load_bps = clamp_bps(round_half_up(weighted, 100U));

    Assessment assessment;
    assessment.features = features;
    assessment.wear_index_before = current.wear_index;
    assessment.wear_increment = static_cast<std::uint32_t>(
        4U + round_half_up(6U * features.episode_load_bps, 10000U));
    assessment.wear_index_after = std::min<std::uint32_t>(
        100U, assessment.wear_index_before + assessment.wear_increment);
    assessment.condition_score = 100U - assessment.wear_index_after;
    assessment.previous_band = current.condition_band;
    assessment.current_band = band_for_score(assessment.condition_score);
    assessment.source_window_start_timestamp = episode.samples.front().source_timestamp;
    assessment.source_window_end_timestamp = episode.samples.back().source_timestamp;

    Evaluation result;
    result.assessment = assessment;
    result.next_state = current;
    result.next_state.generation = current.generation + 1U;
    result.next_state.wear_index = assessment.wear_index_after;
    result.next_state.condition_score = assessment.condition_score;
    result.next_state.condition_band = assessment.current_band;
    return result;
}

}  // namespace brake_health::v2
