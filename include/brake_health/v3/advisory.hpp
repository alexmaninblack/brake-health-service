// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "brake_health/service_identity.hpp"

#include "brake_health/v2/messages.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace brake_health::v3 {

inline constexpr const char* kRequestPath =
    "Vehicle.OEM.BrakeHealth.Advisory.Request";
inline constexpr const char* kStatusPath =
    "Vehicle.OEM.BrakeHealth.Advisory.GatewayStatus";
inline constexpr const char* kReadinessPath =
    "Vehicle.OEM.BrakeHealth.Advisory.Readiness";
inline constexpr const char* kRequestNamespace =
    "894e102e-5380-5c9d-a6f7-46f00b234725";
inline constexpr std::uint64_t kLeaseMilliseconds = 30000U;
inline constexpr std::uint64_t kRefreshMilliseconds = 20000U;

enum class GatewayState {
    Received,
    Applied,
    Cleared,
    Rejected,
    Expired,
    Failed,
};

enum class GatewayReason {
    None,
    UnauthorizedSource,
    UnauthorizedPath,
    InvalidSchema,
    InvalidValue,
    StaleRequest,
    ReplayDetected,
    SequenceRollback,
    RateLimited,
    QmPolicyDenied,
    InternalError,
};

enum class ActiveRecommendation { None, InspectionRecommended };
enum class ActiveReason { None, PredictedBrakeDegradation };

struct DeploymentMetadata {
    std::string unit_system_uid;
    brake_health::v2::UnitRole unit_role{brake_health::v2::UnitRole::Validation};
    std::string service_version{"3.0.0"};
    std::string service_artifact_sha256;
    std::string vdp_contract_version{"3.0.0"};
    std::string vdp_contract_sha256;
    // Absent only for explicitly retained legacy records/golden fixtures.
    std::optional<ServiceInstance> service_instance{};
};

struct AdvisoryRequest {
    std::string request_id;
    std::string producer_epoch;
    std::uint64_t sequence{};
    std::string decision_id;
    std::string issued_at;
    std::string expires_at;
    std::string canonical_json;
    std::string service_version;
    bool clear{};
};

struct GatewayStatus {
    std::string request_id;
    std::string producer_epoch;
    std::uint64_t sequence{};
    GatewayState state{GatewayState::Received};
    GatewayReason reason{GatewayReason::None};
    std::string gateway_observed_at;
    ActiveRecommendation active_recommendation{ActiveRecommendation::None};
    ActiveReason active_reason{ActiveReason::None};
    std::optional<std::string> active_until;
};

const char* gateway_state_name(GatewayState state);
const char* gateway_reason_name(GatewayReason reason);
const char* active_recommendation_name(ActiveRecommendation value);
const char* active_reason_name(ActiveReason value);
bool application_evidence(GatewayState state) noexcept;

bool canonical_uuid(std::string_view value, char version = '\0') noexcept;
bool canonical_sha256(std::string_view value) noexcept;
bool bounded_identifier(std::string_view value) noexcept;
bool semantic_version(std::string_view value) noexcept;
std::int64_t timestamp_milliseconds(const std::string& value);
std::string timestamp_from_milliseconds(std::int64_t value);

}  // namespace brake_health::v3
