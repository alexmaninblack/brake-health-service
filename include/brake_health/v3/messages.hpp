// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "brake_health/v3/advisory.hpp"

#include <string>
#include <string_view>

namespace brake_health::v3 {

struct AdvisoryFact {
    std::string request_id;
    GatewayState gateway_state{GatewayState::Received};
    std::string canonical_json;
    std::string content_sha256;
    std::string message_key_sha256;
    std::string message_sha256;
};

AdvisoryRequest build_set_request(
    const std::string& producer_epoch,
    std::uint64_t sequence,
    const std::string& decision_id,
    const std::string& issued_at,
    const std::string& service_version);
void validate_request(const AdvisoryRequest& request);
std::string gateway_status_json(const GatewayStatus& status);
void validate_gateway_status(const GatewayStatus& status);
AdvisoryFact build_advisory_fact(
    const DeploymentMetadata& metadata,
    const AdvisoryRequest& request,
    const GatewayStatus& status,
    const std::string& recorded_at);
std::string advisory_fact_message_key_sha256(
    const std::string& unit_system_uid,
    const std::string& request_id,
    GatewayState state);

}  // namespace brake_health::v3
