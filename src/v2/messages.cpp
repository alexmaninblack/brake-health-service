// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0

#include "brake_health/v2/messages.hpp"

#include "brake_health/v1/model.hpp"
#include "brake_health/v1/sha256.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace brake_health::v2 {
namespace {

constexpr std::size_t kMaximumMessageBytes = 16384U;
constexpr const char* kAssessmentNamespace = "7a879c3c-0f87-5b08-9d41-f49ab9d4e052";
constexpr const char* kEventNamespace = "b5942c7e-8767-5e34-a432-c5fc7b606a49";

std::uint32_t rotate_left(std::uint32_t value, unsigned count) {
    return (value << count) | (value >> (32U - count));
}

std::array<std::uint8_t, 20> sha1(std::string_view input) {
    std::vector<std::uint8_t> message(input.begin(), input.end());
    const std::uint64_t bit_length = static_cast<std::uint64_t>(message.size()) * 8U;
    message.push_back(0x80U);
    while (message.size() % 64U != 56U) {
        message.push_back(0U);
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
        message.push_back(static_cast<std::uint8_t>((bit_length >> shift) & 0xffU));
    }

    std::array<std::uint32_t, 5> state = {
        0x67452301U, 0xefcdab89U, 0x98badcfeU, 0x10325476U, 0xc3d2e1f0U};
    for (std::size_t offset = 0; offset < message.size(); offset += 64U) {
        std::array<std::uint32_t, 80> words{};
        for (std::size_t index = 0; index < 16U; ++index) {
            const std::size_t at = offset + index * 4U;
            words[index] = (static_cast<std::uint32_t>(message[at]) << 24U) |
                           (static_cast<std::uint32_t>(message[at + 1U]) << 16U) |
                           (static_cast<std::uint32_t>(message[at + 2U]) << 8U) |
                           static_cast<std::uint32_t>(message[at + 3U]);
        }
        for (std::size_t index = 16U; index < words.size(); ++index) {
            words[index] = rotate_left(
                words[index - 3U] ^ words[index - 8U] ^ words[index - 14U] ^
                    words[index - 16U],
                1U);
        }

        std::uint32_t a = state[0];
        std::uint32_t b = state[1];
        std::uint32_t c = state[2];
        std::uint32_t d = state[3];
        std::uint32_t e = state[4];
        for (std::size_t index = 0; index < words.size(); ++index) {
            std::uint32_t function{};
            std::uint32_t constant{};
            if (index < 20U) {
                function = (b & c) | ((~b) & d);
                constant = 0x5a827999U;
            } else if (index < 40U) {
                function = b ^ c ^ d;
                constant = 0x6ed9eba1U;
            } else if (index < 60U) {
                function = (b & c) | (b & d) | (c & d);
                constant = 0x8f1bbcdcU;
            } else {
                function = b ^ c ^ d;
                constant = 0xca62c1d6U;
            }
            const std::uint32_t temporary = rotate_left(a, 5U) + function + e +
                                            constant + words[index];
            e = d;
            d = c;
            c = rotate_left(b, 30U);
            b = a;
            a = temporary;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
    }

    std::array<std::uint8_t, 20> digest{};
    for (std::size_t index = 0; index < state.size(); ++index) {
        digest[index * 4U] = static_cast<std::uint8_t>(state[index] >> 24U);
        digest[index * 4U + 1U] = static_cast<std::uint8_t>(state[index] >> 16U);
        digest[index * 4U + 2U] = static_cast<std::uint8_t>(state[index] >> 8U);
        digest[index * 4U + 3U] = static_cast<std::uint8_t>(state[index]);
    }
    return digest;
}

int hex_value(char character) {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    return -1;
}

std::array<std::uint8_t, 16> decode_uuid(const std::string& value) {
    if (value.size() != 36U || value[8] != '-' || value[13] != '-' ||
        value[18] != '-' || value[23] != '-') {
        throw std::invalid_argument("UUID must use lowercase RFC 4122 text form");
    }
    std::array<std::uint8_t, 16> bytes{};
    std::size_t at = 0U;
    int high = -1;
    for (char character : value) {
        if (character == '-') {
            continue;
        }
        const int nibble = hex_value(character);
        if (nibble < 0) {
            throw std::invalid_argument("UUID must use lowercase hexadecimal digits");
        }
        if (high < 0) {
            high = nibble;
        } else {
            bytes.at(at++) = static_cast<std::uint8_t>((high << 4) | nibble);
            high = -1;
        }
    }
    if (at != bytes.size() || high >= 0) {
        throw std::invalid_argument("invalid UUID byte count");
    }
    return bytes;
}

std::string encode_uuid(const std::array<std::uint8_t, 16>& bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string output;
    output.reserve(36U);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index == 4U || index == 6U || index == 8U || index == 10U) {
            output.push_back('-');
        }
        output.push_back(digits[bytes[index] >> 4U]);
        output.push_back(digits[bytes[index] & 0x0fU]);
    }
    return output;
}

bool uuid_with_version(const std::string& value, char version) {
    try {
        static_cast<void>(decode_uuid(value));
    } catch (const std::invalid_argument&) {
        return false;
    }
    return value[14] == version && std::string("89ab").find(value[19]) != std::string::npos;
}

bool bounded_identifier(const std::string& value) {
    return !value.empty() && value.size() <= 128U &&
           std::all_of(value.begin(), value.end(), [](unsigned char character) {
               return (character >= 'A' && character <= 'Z') ||
                      (character >= 'a' && character <= 'z') ||
                      (character >= '0' && character <= '9') || character == '.' ||
                      character == '_' || character == ':' || character == '-';
           });
}

bool semantic_version(const std::string& value) {
    if (value.empty() || value.size() > 32) return false;
    int dots = 0;
    bool digit = false;
    bool leading_zero = false;
    for (char character : value) {
        if (character >= '0' && character <= '9') {
            if (digit && leading_zero) return false;
            if (!digit) leading_zero = character == '0';
            digit = true;
        } else if (character == '.' && digit && dots < 2) {
            ++dots;
            digit = false;
        } else {
            return false;
        }
    }
    return dots == 2 && digit;
}

bool lowercase_sha256(const std::string& value) {
    return value.size() == 64U &&
           std::all_of(value.begin(), value.end(), [](char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f');
           });
}

std::string json_string(std::string_view value) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string output{"\""};
    for (unsigned char character : value) {
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

std::string scaled_decimal(std::int64_t value, std::uint64_t scale) {
    if (scale == 0U) {
        throw std::invalid_argument("decimal scale must be positive");
    }
    const bool negative = value < 0;
    const std::uint64_t magnitude = negative
        ? static_cast<std::uint64_t>(-(value + 1)) + 1U
        : static_cast<std::uint64_t>(value);
    const std::uint64_t whole = magnitude / scale;
    std::uint64_t fraction = magnitude % scale;
    std::string output = negative ? "-" : "";
    output += std::to_string(whole);
    if (fraction == 0U) {
        return output;
    }
    std::size_t digits = 0U;
    for (std::uint64_t divisor = scale; divisor > 1U; divisor /= 10U) {
        ++digits;
    }
    std::string tail = std::to_string(fraction);
    if (tail.size() < digits) {
        tail.insert(0U, digits - tail.size(), '0');
    }
    while (!tail.empty() && tail.back() == '0') {
        tail.pop_back();
    }
    output.push_back('.');
    output += tail;
    return output;
}

void validate_metadata(
    const DeploymentMetadata& metadata,
    const CompletedEpisode& episode,
    const Assessment& assessment) {
    if ((metadata.service_instance && (!native_identifier(metadata.unit_system_uid) || !package_version(metadata.vdp_contract_version))) ||
        !bounded_identifier(metadata.unit_system_uid) ||
        !semantic_version(metadata.service_version) ||
        !semantic_version(metadata.vdp_contract_version) ||
        (metadata.service_instance ? !native_provenance_valid(metadata.service_instance, metadata.service_version, metadata.service_artifact_sha256) : !lowercase_sha256(metadata.service_artifact_sha256)) ||
        !lowercase_sha256(metadata.vdp_contract_sha256) ||
        (metadata.service_instance ? !metadata.model_artifact_sha256.empty() : !lowercase_sha256(metadata.model_artifact_sha256)) ||
        metadata.model_config_sha256 != kModelConfigSha256 ||
        !uuid_with_version(episode.source_event_id, '4') ||
        !brake_health::v1::is_rfc3339_millisecond_utc(metadata.assessed_at) ||
        !brake_health::v1::is_rfc3339_millisecond_utc(
            assessment.source_window_start_timestamp) ||
        !brake_health::v1::is_rfc3339_millisecond_utc(
            assessment.source_window_end_timestamp) ||
        metadata.assessed_at < assessment.source_window_end_timestamp) {
        throw std::invalid_argument("v2 message metadata does not satisfy the contract");
    }
}

std::string assessment_content(const Assessment& value) {
    const FeatureVector& feature = value.features;
    return std::string("{") +
           "\"activeSampleCount\":" + std::to_string(feature.active_sample_count) +
           ",\"conditionScore\":" + std::to_string(value.condition_score) +
           ",\"currentBand\":" + json_string(condition_band_name(value.current_band)) +
           ",\"episodeLoadBps\":" + std::to_string(feature.episode_load_bps) +
           ",\"features\":{" +
           "\"activeDurationBps\":" + std::to_string(feature.active_duration_bps) +
           ",\"activeDurationSeconds\":" +
           scaled_decimal(feature.active_duration_milliseconds, 1000U) +
           ",\"meanBrakeEffortBps\":" + std::to_string(feature.mean_brake_bps) +
           ",\"meanBrakeEffortPercent\":" +
           scaled_decimal(feature.mean_brake_milli_percent, 1000U) +
           ",\"peakDecelerationBps\":" +
           std::to_string(feature.peak_deceleration_bps) +
           ",\"peakDecelerationMps2\":" +
           scaled_decimal(feature.peak_deceleration_milli_mps2, 1000U) +
           ",\"speedReductionBps\":" + std::to_string(feature.speed_reduction_bps) +
           ",\"speedReductionKph\":" +
           scaled_decimal(feature.speed_reduction_milli_kph, 1000U) +
           ",\"wheelDispersionBps\":" +
           std::to_string(feature.wheel_dispersion_bps) +
           ",\"wheelDispersionRatio\":" +
           scaled_decimal(feature.wheel_dispersion_raw_bps, 10000U) + "}" +
           ",\"previousBand\":" + json_string(condition_band_name(value.previous_band)) +
           ",\"quality\":\"VALID_DEMO_SYNTHETIC\"" +
           ",\"sourceWindowEndTimestamp\":" +
           json_string(value.source_window_end_timestamp) +
           ",\"sourceWindowStartTimestamp\":" +
           json_string(value.source_window_start_timestamp) +
           ",\"straightActiveSampleCount\":" +
           std::to_string(feature.straight_active_sample_count) +
           ",\"wearIncrement\":" + std::to_string(value.wear_increment) +
           ",\"wearIndexAfter\":" + std::to_string(value.wear_index_after) +
           ",\"wearIndexBefore\":" + std::to_string(value.wear_index_before) + "}";
}

std::string event_content(const Assessment& value) {
    return std::string("{") +
           "\"conditionScore\":" + std::to_string(value.condition_score) +
           ",\"currentBand\":" + json_string(condition_band_name(value.current_band)) +
           ",\"effectiveAt\":" + json_string(value.source_window_end_timestamp) +
           ",\"eventType\":\"BRAKE_CONDITION_BAND_CHANGED\"" +
           ",\"previousBand\":" + json_string(condition_band_name(value.previous_band)) +
           ",\"quality\":\"VALID_DEMO_SYNTHETIC\"" +
           ",\"reasonCode\":\"SYNTHETIC_ACCUMULATED_STRESS_THRESHOLD\"}";
}

std::string make_idempotency_digest(
    const std::string& unit_system_uid,
    const std::string& message_type,
    const std::string& id) {
    const std::string key = "[" + json_string(unit_system_uid) + "," +
                            json_string(message_type) + "," + json_string(id) + "]";
    return brake_health::v1::sha256_hex(key);
}

void enforce_message_size(const std::string& value) {
    if (value.size() > kMaximumMessageBytes) {
        throw std::length_error("canonical v2 message exceeds 16 KiB");
    }
}

std::size_t field_value_at(std::string_view json, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\":";
    const std::size_t at = json.find(needle);
    if (at == std::string_view::npos || json.find(needle, at + 1U) != std::string_view::npos) {
        throw std::invalid_argument("state field is missing or duplicated");
    }
    return at + needle.size();
}

std::string parse_simple_string(std::string_view json, std::string_view key) {
    std::size_t at = field_value_at(json, key);
    if (at >= json.size() || json[at] != '"') {
        throw std::invalid_argument("state string field is malformed");
    }
    const std::size_t end = json.find('"', at + 1U);
    if (end == std::string_view::npos) {
        throw std::invalid_argument("unterminated state string");
    }
    const std::string value(json.substr(at + 1U, end - at - 1U));
    if (value.find('\\') != std::string::npos) {
        throw std::invalid_argument("escaped state identifiers are forbidden");
    }
    return value;
}

std::optional<std::string> parse_optional_string(
    std::string_view json,
    std::string_view key) {
    const std::size_t at = field_value_at(json, key);
    if (json.substr(at, 4U) == "null") {
        return std::nullopt;
    }
    return parse_simple_string(json, key);
}

std::uint64_t parse_unsigned(std::string_view json, std::string_view key) {
    const std::size_t at = field_value_at(json, key);
    const char* first = json.data() + at;
    const char* last = first;
    while (last != json.data() + json.size() && *last >= '0' && *last <= '9') {
        ++last;
    }
    if (first == last || (last - first > 1 && *first == '0')) {
        throw std::invalid_argument("state integer field is malformed");
    }
    std::uint64_t value{};
    const auto result = std::from_chars(first, last, value);
    if (result.ec != std::errc{} || result.ptr != last) {
        throw std::invalid_argument("state integer is out of range");
    }
    return value;
}

std::vector<std::string> parse_string_array(
    std::string_view json,
    std::string_view key) {
    std::size_t at = field_value_at(json, key);
    if (at >= json.size() || json[at++] != '[') {
        throw std::invalid_argument("state array is malformed");
    }
    std::vector<std::string> result;
    if (at < json.size() && json[at] == ']') {
        return result;
    }
    while (at < json.size()) {
        if (json[at++] != '"') {
            throw std::invalid_argument("state array item is malformed");
        }
        const std::size_t end = json.find('"', at);
        if (end == std::string_view::npos) {
            throw std::invalid_argument("state array item is unterminated");
        }
        result.emplace_back(json.substr(at, end - at));
        at = end + 1U;
        if (at < json.size() && json[at] == ']') {
            return result;
        }
        if (at >= json.size() || json[at++] != ',') {
            throw std::invalid_argument("state array delimiter is malformed");
        }
    }
    throw std::invalid_argument("state array is unterminated");
}

ConditionBand parse_band(const std::string& value) {
    if (value == "GOOD") return ConditionBand::Good;
    if (value == "MONITOR") return ConditionBand::Monitor;
    if (value == "INSPECTION_RECOMMENDED") return ConditionBand::InspectionRecommended;
    throw std::invalid_argument("unknown condition band");
}

void validate_state(const ModelState& state) {
    if (!supported_model_config(state.model_config_sha256) ||
        state.wear_index > 100U || state.condition_score != 100U - state.wear_index ||
        state.condition_band != band_for_score(state.condition_score) ||
        !uuid_with_version(state.producer_epoch, '4') ||
        state.next_advisory_sequence == 0U || state.recent_source_event_ids.size() > 64U ||
        (state.last_applied_source_event_id &&
         !uuid_with_version(*state.last_applied_source_event_id, '4')) ||
        (state.last_assessment_id && !uuid_with_version(*state.last_assessment_id, '5'))) {
        throw std::invalid_argument("v2 model state violates the closed schema");
    }
    std::set<std::string> unique;
    for (const std::string& id : state.recent_source_event_ids) {
        if (!uuid_with_version(id, '4') || !unique.insert(id).second) {
            throw std::invalid_argument("recent source-event ledger is invalid");
        }
    }
    if (state.last_applied_source_event_id &&
        unique.find(*state.last_applied_source_event_id) == unique.end()) {
        throw std::invalid_argument("last source event is absent from recent ledger");
    }
    if (state.last_applied_source_event_id.has_value() != state.last_assessment_id.has_value()) {
        throw std::invalid_argument("last source-event and assessment identities must pair");
    }
}

}  // namespace

std::string uuid_v5(
    const std::string& namespace_uuid,
    const std::vector<std::string>& fields) {
    std::string bytes;
    const auto namespace_bytes = decode_uuid(namespace_uuid);
    bytes.assign(
        reinterpret_cast<const char*>(namespace_bytes.data()), namespace_bytes.size());
    for (std::size_t index = 0; index < fields.size(); ++index) {
        if (fields[index].find_first_of(std::string("\0\n\r", 3U)) != std::string::npos) {
            throw std::invalid_argument("UUIDv5 field contains a forbidden delimiter byte");
        }
        if (index != 0U) {
            bytes.push_back('\n');
        }
        bytes += fields[index];
    }
    const auto digest = sha1(bytes);
    std::array<std::uint8_t, 16> uuid{};
    std::copy_n(digest.begin(), uuid.size(), uuid.begin());
    uuid[6] = static_cast<std::uint8_t>((uuid[6] & 0x0fU) | 0x50U);
    uuid[8] = static_cast<std::uint8_t>((uuid[8] & 0x3fU) | 0x80U);
    return encode_uuid(uuid);
}

std::string assessment_id(
    const DeploymentMetadata& metadata,
    const std::string& source_event_id) {
    return uuid_v5(
        kAssessmentNamespace,
        {metadata.unit_system_uid, source_event_id, metadata.model_config_sha256});
}

std::string message_idempotency_key_sha256(
    const std::string& unit_system_uid,
    const std::string& message_type,
    const std::string& id) {
    if (!bounded_identifier(unit_system_uid) ||
        (message_type != "BRAKE_HEALTH_ASSESSMENT" &&
         message_type != "BRAKE_HEALTH_EVENT") ||
        !uuid_with_version(id, '5')) {
        throw std::invalid_argument("idempotency identity does not satisfy the contract");
    }
    return make_idempotency_digest(unit_system_uid, message_type, id);
}

DerivedMessages build_messages(
    const DeploymentMetadata& metadata,
    const CompletedEpisode& episode,
    const Assessment& assessment) {
    validate_metadata(metadata, episode, assessment);
    const std::string id = assessment_id(metadata, episode.source_event_id);
    const std::string content = assessment_content(assessment);
    const std::string content_sha = brake_health::v1::sha256_hex(content);
    std::string encoded = std::string("{") +
        "\"assessedAt\":" + json_string(metadata.assessed_at) +
        ",\"assessmentId\":" + json_string(id) +
        ",\"content\":" + content +
        ",\"contentSha256\":" + json_string(content_sha) +
        (metadata.service_instance ? ",\"contractVersion\":\"2.0.0\"" : ",\"contractVersion\":\"1.0.0\"") +
        ",\"messageType\":\"BRAKE_HEALTH_ASSESSMENT\"" +
        (metadata.service_instance ? "" : ",\"modelArtifactSha256\":" + json_string(metadata.model_artifact_sha256)) +
        ",\"modelConfigSha256\":" + json_string(metadata.model_config_sha256) +
        ",\"modelId\":\"brake-condition-demo-v1\"" +
        ",\"modelVersion\":\"1.0.0\"" +
        ",\"provenance\":\"DEMO_SYNTHETIC\"" +
        (metadata.service_instance ? ",\"schemaVersion\":2" : ",\"schemaVersion\":1") +
        (metadata.service_instance ? ",\"serviceInstance\":" + service_instance_json(*metadata.service_instance) :
            ",\"serviceArtifactSha256\":" + json_string(metadata.service_artifact_sha256)) +
        ",\"serviceVersion\":" + json_string(metadata.service_version) +
        ",\"sourceEventId\":" + json_string(episode.source_event_id) +
        ",\"unitRole\":" + json_string(unit_role_name(metadata.unit_role)) +
        ",\"unitSystemUid\":" + json_string(metadata.unit_system_uid) +
        ",\"vdpContractSha256\":" + json_string(metadata.vdp_contract_sha256) +
        ",\"vdpContractVersion\":" + json_string(metadata.vdp_contract_version) + "}";
    enforce_message_size(encoded);

    DerivedMessages result;
    result.assessment = {
        id,
        "BRAKE_HEALTH_ASSESSMENT",
        std::move(encoded),
        content_sha,
        message_idempotency_key_sha256(
            metadata.unit_system_uid, "BRAKE_HEALTH_ASSESSMENT", id)};

    if (assessment.previous_band != assessment.current_band) {
        const std::string event_id = uuid_v5(
            kEventNamespace,
            {id, "BRAKE_CONDITION_BAND_CHANGED",
             condition_band_name(assessment.current_band)});
        const std::string event_body = event_content(assessment);
        const std::string event_sha = brake_health::v1::sha256_hex(event_body);
        std::string event_encoded = std::string("{") +
            "\"assessmentId\":" + json_string(id) +
            ",\"content\":" + event_body +
            ",\"contentSha256\":" + json_string(event_sha) +
            (metadata.service_instance ? ",\"contractVersion\":\"2.0.0\"" : ",\"contractVersion\":\"1.0.0\"") +
            ",\"eventId\":" + json_string(event_id) +
            ",\"messageType\":\"BRAKE_HEALTH_EVENT\"" +
            ",\"modelConfigSha256\":" + json_string(metadata.model_config_sha256) +
            ",\"modelId\":\"brake-condition-demo-v1\"" +
            ",\"modelVersion\":\"1.0.0\"" +
            ",\"provenance\":\"DEMO_SYNTHETIC\"" +
            (metadata.service_instance ? ",\"schemaVersion\":2" : ",\"schemaVersion\":1") +
            (metadata.service_instance ? ",\"serviceInstance\":" + service_instance_json(*metadata.service_instance) :
            ",\"serviceArtifactSha256\":" + json_string(metadata.service_artifact_sha256)) +
            ",\"serviceVersion\":" + json_string(metadata.service_version) +
            ",\"sourceEventId\":" + json_string(episode.source_event_id) +
            ",\"unitRole\":" + json_string(unit_role_name(metadata.unit_role)) +
            ",\"unitSystemUid\":" + json_string(metadata.unit_system_uid) + "}";
        enforce_message_size(event_encoded);
        result.event = CanonicalMessage{
            event_id,
            "BRAKE_HEALTH_EVENT",
            std::move(event_encoded),
            event_sha,
            message_idempotency_key_sha256(
                metadata.unit_system_uid, "BRAKE_HEALTH_EVENT", event_id)};
    }
    return result;
}

std::string state_json(const ModelState& state) {
    validate_state(state);
    std::string recent = "[";
    for (std::size_t index = 0; index < state.recent_source_event_ids.size(); ++index) {
        if (index != 0U) recent.push_back(',');
        recent += json_string(state.recent_source_event_ids[index]);
    }
    recent.push_back(']');
    const std::string last_event = state.last_applied_source_event_id
        ? json_string(*state.last_applied_source_event_id) : "null";
    const std::string last_assessment = state.last_assessment_id
        ? json_string(*state.last_assessment_id) : "null";
    return std::string("{") +
        "\"conditionBand\":" + json_string(condition_band_name(state.condition_band)) +
        ",\"conditionScore\":" + std::to_string(state.condition_score) +
        ",\"generation\":" + std::to_string(state.generation) +
        ",\"lastAppliedSourceEventId\":" + last_event +
        ",\"lastAssessmentId\":" + last_assessment +
        ",\"modelConfigSha256\":\"" + state.model_config_sha256 + "\"" +
        ",\"modelId\":\"brake-condition-demo-v1\"" +
        ",\"modelVersion\":\"1.0.0\"" +
        ",\"nextAdvisorySequence\":" + std::to_string(state.next_advisory_sequence) +
        ",\"producerEpoch\":" + json_string(state.producer_epoch) +
        ",\"profile\":\"DEMO_PRECONDITIONED\"" +
        ",\"recentSourceEventIds\":" + recent +
        ",\"schemaVersion\":1" +
        ",\"wearIndex\":" + std::to_string(state.wear_index) + "}";
}

ModelState parse_state_json(std::string_view json) {
    if (json.size() > 65536U ||
        parse_unsigned(json, "schemaVersion") != 1U ||
        parse_simple_string(json, "modelId") != kModelId ||
        parse_simple_string(json, "modelVersion") != kModelVersion ||
        !supported_model_config(parse_simple_string(json, "modelConfigSha256")) ||
        parse_simple_string(json, "profile") != kModelProfile) {
        throw std::invalid_argument("unknown v2 state schema or model identity");
    }
    ModelState state;
    state.model_config_sha256 = parse_simple_string(json, "modelConfigSha256");
    state.generation = parse_unsigned(json, "generation");
    const std::uint64_t wear = parse_unsigned(json, "wearIndex");
    const std::uint64_t score = parse_unsigned(json, "conditionScore");
    if (wear > std::numeric_limits<std::uint32_t>::max() ||
        score > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("state score exceeds integer range");
    }
    state.wear_index = static_cast<std::uint32_t>(wear);
    state.condition_score = static_cast<std::uint32_t>(score);
    state.condition_band = parse_band(parse_simple_string(json, "conditionBand"));
    state.last_applied_source_event_id = parse_optional_string(
        json, "lastAppliedSourceEventId");
    state.last_assessment_id = parse_optional_string(json, "lastAssessmentId");
    state.recent_source_event_ids = parse_string_array(json, "recentSourceEventIds");
    state.producer_epoch = parse_simple_string(json, "producerEpoch");
    state.next_advisory_sequence = parse_unsigned(json, "nextAdvisorySequence");
    validate_state(state);
    if (state_json(state) != json) {
        throw std::invalid_argument("state is not the exact canonical closed-schema encoding");
    }
    return state;
}

const char* unit_role_name(UnitRole role) {
    switch (role) {
        case UnitRole::Validation: return "VALIDATION";
        case UnitRole::Production: return "PRODUCTION";
    }
    throw std::invalid_argument("unknown Unit role");
}

}  // namespace brake_health::v2
