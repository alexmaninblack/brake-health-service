// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0

#include "brake_health/v3/messages.hpp"

#include "brake_health/v1/sha256.hpp"
#include "brake_health/v2/messages.hpp"

#include <limits>
#include <stdexcept>

namespace brake_health::v3 {
namespace {

constexpr std::size_t kMaximumRequestBytes = 2048U;
constexpr std::size_t kMaximumStatusBytes = 1024U;
constexpr std::size_t kMaximumFactBytes = 16384U;

std::string json_string(std::string_view value) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string output{"\""};
    for (const unsigned char character : value) {
        switch (character) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (character < 0x20U) {
                    output += "\\u00";
                    output.push_back(digits[character >> 4U]);
                    output.push_back(digits[character & 0x0fU]);
                } else {
                    output.push_back(static_cast<char>(character));
                }
        }
    }
    output.push_back('"');
    return output;
}

void validate_metadata(const DeploymentMetadata& metadata) {
    if ((metadata.service_instance && (!native_identifier(metadata.unit_system_uid) || !package_version(metadata.vdp_contract_version))) ||
        !bounded_identifier(metadata.unit_system_uid) ||
        !semantic_version(metadata.service_version) ||
        (metadata.service_instance ? !native_provenance_valid(metadata.service_instance, metadata.service_version, metadata.service_artifact_sha256) : !canonical_sha256(metadata.service_artifact_sha256)) ||
        !semantic_version(metadata.vdp_contract_version) ||
        !canonical_sha256(metadata.vdp_contract_sha256)) {
        throw std::invalid_argument("v3 deployment metadata violates the contract");
    }
}

std::string request_bytes(
    const std::string& request_id,
    const std::string& producer_epoch,
    std::uint64_t sequence,
    const std::string& decision_id,
    const std::string& issued_at,
    const std::string& expires_at,
    const std::string& service_version) {
    return std::string("{") +
        "\"decisionId\":" + json_string(decision_id) +
        ",\"expiresAt\":" + json_string(expires_at) +
        ",\"issuedAt\":" + json_string(issued_at) +
        ",\"modelVersion\":\"brake-condition-demo-v1\"" +
        ",\"operation\":\"SET\"" +
        ",\"producerEpoch\":" + json_string(producer_epoch) +
        ",\"reasonCode\":\"PREDICTED_BRAKE_DEGRADATION\"" +
        ",\"recommendation\":\"INSPECTION_RECOMMENDED\"" +
        ",\"requestId\":" + json_string(request_id) +
        ",\"schemaVersion\":1" +
        ",\"sequence\":" + std::to_string(sequence) +
        ",\"serviceVersion\":" + json_string(service_version) + "}";
}

}  // namespace

AdvisoryRequest build_set_request(
    const std::string& producer_epoch,
    std::uint64_t sequence,
    const std::string& decision_id,
    const std::string& issued_at,
    const std::string& service_version) {
    if (!canonical_uuid(producer_epoch, '4') || sequence == 0U ||
        !bounded_identifier(decision_id) || !canonical_uuid(decision_id, '5') ||
        !semantic_version(service_version)) {
        throw std::invalid_argument("advisory request identity violates the contract");
    }
    const std::int64_t issued_milliseconds = timestamp_milliseconds(issued_at);
    if (issued_milliseconds >
        std::numeric_limits<std::int64_t>::max() -
            static_cast<std::int64_t>(kLeaseMilliseconds)) {
        throw std::out_of_range("advisory lease timestamp overflows");
    }
    const std::string expires_at = timestamp_from_milliseconds(
        issued_milliseconds + static_cast<std::int64_t>(kLeaseMilliseconds));
    const std::string request_id = brake_health::v2::uuid_v5(
        kRequestNamespace,
        {producer_epoch, std::to_string(sequence), "SET", decision_id});
    AdvisoryRequest result{
        request_id,
        producer_epoch,
        sequence,
        decision_id,
        issued_at,
        expires_at,
        request_bytes(
            request_id, producer_epoch, sequence, decision_id, issued_at,
            expires_at, service_version), service_version};
    validate_request(result);
    return result;
}

void validate_request(const AdvisoryRequest& request) {
    if (!canonical_uuid(request.request_id, '5') ||
        !canonical_uuid(request.producer_epoch, '4') || request.sequence == 0U ||
        !canonical_uuid(request.decision_id, '5') || !semantic_version(request.service_version) ||
        timestamp_milliseconds(request.expires_at) -
                timestamp_milliseconds(request.issued_at) !=
            static_cast<std::int64_t>(kLeaseMilliseconds) ||
        brake_health::v2::uuid_v5(
            kRequestNamespace,
            {request.producer_epoch, std::to_string(request.sequence), "SET",
             request.decision_id}) != request.request_id ||
        request_bytes(
            request.request_id, request.producer_epoch, request.sequence,
            request.decision_id, request.issued_at, request.expires_at, request.service_version) !=
            request.canonical_json ||
        request.canonical_json.size() > kMaximumRequestBytes) {
        throw std::invalid_argument("advisory request is not canonical");
    }
}

void validate_gateway_status(const GatewayStatus& status) {
    if (!canonical_uuid(status.request_id, '5') ||
        !canonical_uuid(status.producer_epoch, '4') || status.sequence == 0U) {
        throw std::invalid_argument("Gateway Status identity violates the contract");
    }
    static_cast<void>(timestamp_milliseconds(status.gateway_observed_at));
    const bool active =
        status.active_recommendation == ActiveRecommendation::InspectionRecommended &&
        status.active_reason == ActiveReason::PredictedBrakeDegradation &&
        status.active_until.has_value();
    const bool inactive =
        status.active_recommendation == ActiveRecommendation::None &&
        status.active_reason == ActiveReason::None && !status.active_until.has_value();
    if (!active && !inactive) {
        throw std::invalid_argument("Gateway Status active fields are inconsistent");
    }
    if (status.active_until) {
        static_cast<void>(timestamp_milliseconds(*status.active_until));
    }
    if (status.state == GatewayState::Applied && !active) {
        throw std::invalid_argument("APPLIED Gateway Status lacks the active advisory");
    }
    if (status.state == GatewayState::Cleared && !inactive) {
        throw std::invalid_argument("CLEARED Gateway Status retains an active advisory");
    }
    static_cast<void>(gateway_state_name(status.state));
    static_cast<void>(gateway_reason_name(status.reason));
}

std::string gateway_status_json(const GatewayStatus& status) {
    validate_gateway_status(status);
    const std::string active_until = status.active_until
        ? json_string(*status.active_until) : "null";
    const std::string result = std::string("{") +
        "\"activeReasonCode\":" + json_string(active_reason_name(status.active_reason)) +
        ",\"activeRecommendation\":" +
        json_string(active_recommendation_name(status.active_recommendation)) +
        ",\"activeUntil\":" + active_until +
        ",\"gatewayObservedAt\":" + json_string(status.gateway_observed_at) +
        ",\"producerEpoch\":" + json_string(status.producer_epoch) +
        ",\"reason\":" + json_string(gateway_reason_name(status.reason)) +
        ",\"requestId\":" + json_string(status.request_id) +
        ",\"schemaVersion\":1" +
        ",\"sequence\":" + std::to_string(status.sequence) +
        ",\"state\":" + json_string(gateway_state_name(status.state)) + "}";
    if (result.size() > kMaximumStatusBytes) {
        throw std::length_error("Gateway Status exceeds 1 KiB");
    }
    return result;
}

std::string advisory_fact_message_key_sha256(
    const std::string& unit_system_uid,
    const std::string& request_id,
    GatewayState state) {
    if (!bounded_identifier(unit_system_uid) || !canonical_uuid(request_id, '5')) {
        throw std::invalid_argument("advisory fact identity violates the contract");
    }
    const std::string key = "[" + json_string(unit_system_uid) +
        ",\"BRAKE_ADVISORY_FACT\"," + json_string(request_id) + "," +
        json_string(gateway_state_name(state)) + "]";
    return brake_health::v1::sha256_hex(key);
}

AdvisoryFact build_advisory_fact(
    const DeploymentMetadata& metadata,
    const AdvisoryRequest& request,
    const GatewayStatus& status,
    const std::string& recorded_at) {
    validate_metadata(metadata);
    validate_request(request);
    validate_gateway_status(status);
    static_cast<void>(timestamp_milliseconds(recorded_at));
    if (request.request_id != status.request_id ||
        request.producer_epoch != status.producer_epoch ||
        request.sequence != status.sequence) {
        throw std::invalid_argument("Gateway Status does not match the committed Request");
    }
    const std::string active_until = status.active_until
        ? json_string(*status.active_until) : "null";
    const std::string canonical_content = std::string("{") +
        "\"activeReasonCode\":" + json_string(active_reason_name(status.active_reason)) +
        ",\"activeRecommendation\":" +
        json_string(active_recommendation_name(status.active_recommendation)) +
        ",\"activeUntil\":" + active_until +
        ",\"decisionId\":" + json_string(request.decision_id) +
        ",\"expiresAt\":" + json_string(request.expires_at) +
        ",\"gatewayObservedAt\":" + json_string(status.gateway_observed_at) +
        ",\"gatewayReason\":" + json_string(gateway_reason_name(status.reason)) +
        ",\"issuedAt\":" + json_string(request.issued_at) +
        ",\"operation\":\"SET\"" +
        ",\"reasonCode\":\"PREDICTED_BRAKE_DEGRADATION\"" +
        ",\"recommendation\":\"INSPECTION_RECOMMENDED\"}";
    const std::string content_sha = brake_health::v1::sha256_hex(canonical_content);
    const std::string encoded = std::string("{") +
        "\"content\":" + canonical_content +
        ",\"contentSha256\":" + json_string(content_sha) +
        (metadata.service_instance ? ",\"contractVersion\":\"2.0.0\"" : ",\"contractVersion\":\"1.0.0\"") +
        ",\"gatewayState\":" + json_string(gateway_state_name(status.state)) +
        ",\"messageType\":\"BRAKE_ADVISORY_FACT\"" +
        ",\"producerEpoch\":" + json_string(request.producer_epoch) +
        ",\"recordedAt\":" + json_string(recorded_at) +
        ",\"requestId\":" + json_string(request.request_id) +
        (metadata.service_instance ? ",\"schemaVersion\":2" : ",\"schemaVersion\":1") +
        ",\"sequence\":" + std::to_string(request.sequence) +
        (metadata.service_instance ? ",\"serviceInstance\":" + service_instance_json(*metadata.service_instance) :
            ",\"serviceArtifactSha256\":" + json_string(metadata.service_artifact_sha256)) +
        ",\"serviceVersion\":" + json_string(metadata.service_version) +
        ",\"unitRole\":" +
        json_string(brake_health::v2::unit_role_name(metadata.unit_role)) +
        ",\"unitSystemUid\":" + json_string(metadata.unit_system_uid) +
        ",\"vdpContractSha256\":" + json_string(metadata.vdp_contract_sha256) +
        ",\"vdpContractVersion\":" + json_string(metadata.vdp_contract_version) + "}";
    if (encoded.size() > kMaximumFactBytes) {
        throw std::length_error("canonical advisory fact exceeds 16 KiB");
    }
    return {
        request.request_id,
        status.state,
        encoded,
        content_sha,
        advisory_fact_message_key_sha256(
            metadata.unit_system_uid, request.request_id, status.state),
        brake_health::v1::sha256_hex(encoded)};
}

}  // namespace brake_health::v3
