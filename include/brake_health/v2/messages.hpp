// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "brake_health/v2/model.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace brake_health::v2 {

enum class UnitRole { Validation, Production };

struct DeploymentMetadata {
    std::string unit_system_uid;
    UnitRole unit_role{UnitRole::Validation};
    std::string service_version{"2.0.0"};
    std::string service_artifact_sha256;
    std::string vdp_contract_version{"2.0.0"};
    std::string vdp_contract_sha256;
    std::string model_artifact_sha256;
    std::string model_config_sha256{kModelConfigSha256};
    std::string assessed_at;
};

struct CanonicalMessage {
    std::string id;
    std::string message_type;
    std::string canonical_json;
    std::string content_sha256;
    std::string idempotency_key_sha256;
};

struct DerivedMessages {
    CanonicalMessage assessment;
    std::optional<CanonicalMessage> event;

    std::size_t count() const { return event ? 2U : 1U; }
    std::size_t encoded_bytes() const {
        return assessment.canonical_json.size() +
               (event ? event->canonical_json.size() : 0U);
    }
};

std::string uuid_v5(const std::string& namespace_uuid, const std::vector<std::string>& fields);
std::string assessment_id(
    const DeploymentMetadata& metadata,
    const std::string& source_event_id);
std::string message_idempotency_key_sha256(
    const std::string& unit_system_uid,
    const std::string& message_type,
    const std::string& id);
DerivedMessages build_messages(
    const DeploymentMetadata& metadata,
    const CompletedEpisode& episode,
    const Assessment& assessment);
std::string state_json(const ModelState& state);
ModelState parse_state_json(std::string_view json);
const char* unit_role_name(UnitRole role);

}  // namespace brake_health::v2
