// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "brake_health/v2/episode.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace brake_health::v2 {

inline constexpr const char* kModelId = "brake-condition-demo-v1";
inline constexpr const char* kModelVersion = "1.0.0";
inline constexpr const char* kModelProfile = "DEMO_PRECONDITIONED";
inline constexpr const char* kModelConfigSha256 =
    "7749dff2dd340f05ae5f3c90912d65007ad48c52a5136ab0e165a83109d55f53";

enum class ConditionBand { Good, Monitor, InspectionRecommended };

struct ModelState {
    std::uint64_t generation{};
    std::uint32_t wear_index{54};
    std::uint32_t condition_score{46};
    ConditionBand condition_band{ConditionBand::Monitor};
    std::optional<std::string> last_applied_source_event_id;
    std::optional<std::string> last_assessment_id;
    std::vector<std::string> recent_source_event_ids;
    std::string producer_epoch;
    std::uint64_t next_advisory_sequence{1};
};

struct FeatureVector {
    std::int64_t peak_deceleration_milli_mps2{};
    std::uint32_t peak_deceleration_bps{};
    std::uint32_t active_duration_milliseconds{};
    std::uint32_t active_duration_bps{};
    std::int64_t speed_reduction_milli_kph{};
    std::uint32_t speed_reduction_bps{};
    std::int64_t mean_brake_milli_percent{};
    std::uint32_t mean_brake_bps{};
    std::uint32_t wheel_dispersion_raw_bps{};
    std::uint32_t wheel_dispersion_bps{};
    std::uint32_t episode_load_bps{};
    std::uint32_t active_sample_count{};
    std::uint32_t straight_active_sample_count{};
};

struct Assessment {
    FeatureVector features;
    std::uint32_t wear_index_before{};
    std::uint32_t wear_increment{};
    std::uint32_t wear_index_after{};
    std::uint32_t condition_score{};
    ConditionBand previous_band{ConditionBand::Monitor};
    ConditionBand current_band{ConditionBand::Monitor};
    std::string source_window_start_timestamp;
    std::string source_window_end_timestamp;
};

struct Evaluation {
    std::optional<SkipReason> skip_reason;
    std::optional<Assessment> assessment;
    ModelState next_state;
};

class SyntheticModel {
public:
    Evaluation evaluate(const CompletedEpisode& episode, const ModelState& current) const;
};

ModelState initial_state(std::string producer_epoch);
ConditionBand band_for_score(std::uint32_t score);
const char* condition_band_name(ConditionBand band);
std::uint64_t round_half_up(std::uint64_t numerator, std::uint64_t denominator);

}  // namespace brake_health::v2
