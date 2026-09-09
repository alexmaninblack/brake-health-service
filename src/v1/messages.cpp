// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0

#include "brake_health/v1/messages.hpp"

#include "brake_health/v1/model.hpp"
#include "brake_health/v1/sha256.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>

namespace brake_health::v1 {
namespace {

constexpr std::size_t maximum_message_bytes = 65536U;

bool bounded_identifier(const std::string& value) {
    if (value.empty() || value.size() > 128U) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return (character >= 'A' && character <= 'Z') ||
               (character >= 'a' && character <= 'z') ||
               (character >= '0' && character <= '9') || character == '.' ||
               character == '_' || character == ':' || character == '-';
    });
}

bool semantic_version(const std::string& value) {
    int components = 0;
    bool digit_in_component = false;
    for (char character : value) {
        if (character >= '0' && character <= '9') {
            digit_in_component = true;
        } else if (character == '.' && digit_in_component && components < 2) {
            ++components;
            digit_in_component = false;
        } else {
            return false;
        }
    }
    return components == 2 && digit_in_component;
}

bool lowercase_sha256(const std::string& value) {
    return value.size() == 64U &&
           std::all_of(value.begin(), value.end(), [](char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f');
           });
}

bool uuid4(const std::string& value) {
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

std::string json_string(std::string_view value) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string output;
    output.push_back('"');
    for (unsigned char character : value) {
        switch (character) {
            case '"':
                output += "\\\"";
                break;
            case '\\':
                output += "\\\\";
                break;
            case '\b':
                output += "\\b";
                break;
            case '\f':
                output += "\\f";
                break;
            case '\n':
                output += "\\n";
                break;
            case '\r':
                output += "\\r";
                break;
            case '\t':
                output += "\\t";
                break;
            default:
                if (character < 0x20U) {
                    output += "\\u00";
                    output.push_back(hex[character >> 4U]);
                    output.push_back(hex[character & 0x0fU]);
                } else {
                    output.push_back(static_cast<char>(character));
                }
        }
    }
    output.push_back('"');
    return output;
}

std::string canonical_number(double value) {
    if (!std::isfinite(value)) {
        throw std::invalid_argument("canonical JSON does not permit non-finite numbers");
    }
    if (value == 0.0) {
        return "0";
    }
    std::array<char, 64> buffer{};
    const auto result = std::to_chars(
        buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::general);
    if (result.ec != std::errc{}) {
        throw std::runtime_error("failed to serialize canonical number");
    }
    const std::string shortest(buffer.data(), result.ptr);
    const std::size_t exponent_at = shortest.find('e');
    if (exponent_at == std::string::npos) {
        return shortest;
    }

    const bool negative = shortest.front() == '-';
    const std::size_t mantissa_at = negative ? 1U : 0U;
    const std::string mantissa =
        shortest.substr(mantissa_at, exponent_at - mantissa_at);
    const std::size_t dot = mantissa.find('.');
    std::string digits = mantissa;
    if (dot != std::string::npos) {
        digits.erase(dot, 1U);
    }
    const int digits_before_dot =
        static_cast<int>(dot == std::string::npos ? mantissa.size() : dot);
    const int exponent = std::stoi(shortest.substr(exponent_at + 1U));
    const int decimal_position = digits_before_dot + exponent;

    if (std::fabs(value) >= 1e-6 && std::fabs(value) < 1e21) {
        std::string output = negative ? "-" : "";
        if (decimal_position <= 0) {
            output += "0.";
            output.append(static_cast<std::size_t>(-decimal_position), '0');
            output += digits;
        } else if (decimal_position >= static_cast<int>(digits.size())) {
            output += digits;
            output.append(
                static_cast<std::size_t>(decimal_position - static_cast<int>(digits.size())),
                '0');
        } else {
            output += digits.substr(0, static_cast<std::size_t>(decimal_position));
            output.push_back('.');
            output += digits.substr(static_cast<std::size_t>(decimal_position));
        }
        return output;
    }

    std::string output = negative ? "-" : "";
    output.push_back(digits.front());
    if (digits.size() > 1U) {
        output.push_back('.');
        output += digits.substr(1U);
    }
    const int scientific_exponent = decimal_position - 1;
    output.push_back('e');
    if (scientific_exponent >= 0) {
        output.push_back('+');
    }
    output += std::to_string(scientific_exponent);
    return output;
}

std::string sample_json(const RetainedSample& sample, std::size_t sample_index) {
    const SourceFrame& frame = sample.frame;
    if (sample_index > 149U ||
        !FrameValidator{}.validate(frame, std::nullopt, std::nullopt).valid) {
        throw std::invalid_argument("sample does not satisfy the v1 schema");
    }
    return std::string("{") +
           "\"acceleratorPedalPercent\":" + std::to_string(frame.accelerator_pedal_percent) +
           ",\"brakePedalPercent\":" + std::to_string(frame.brake_pedal_percent) +
           ",\"lateralAccelerationMps2\":" +
           canonical_number(frame.lateral_acceleration_mps2) +
           ",\"longitudinalAccelerationMps2\":" +
           canonical_number(frame.longitudinal_acceleration_mps2) +
           ",\"maxSourceAgeMs\":" + std::to_string(frame.max_source_age_ms) +
           ",\"phase\":" + json_string(phase_name(sample.phase)) +
           ",\"quality\":\"VALID_COMPLETE_FRAME\"" +
           ",\"sampleIndex\":" + std::to_string(sample_index) +
           ",\"sourceTimestamp\":" + json_string(frame.source_timestamp) +
           ",\"speedKph\":" + canonical_number(frame.speed_kph) +
           ",\"verticalAccelerationMps2\":" +
           canonical_number(frame.vertical_acceleration_mps2) + "}";
}

std::string top_level_message(
    const MessageMetadata& metadata,
    const EventWindow& window,
    std::string_view message_type,
    std::string_view content,
    std::string_view content_sha256) {
    return std::string("{") +
           "\"content\":" + std::string(content) +
           ",\"contentSha256\":" + json_string(content_sha256) +
           ",\"contractVersion\":\"1.0.0\"" +
           ",\"eventId\":" + json_string(window.event_id) +
           ",\"eventType\":\"HARD_BRAKING_EPISODE_V1\"" +
           ",\"messageType\":" + json_string(message_type) +
           ",\"schemaVersion\":1" +
           ",\"serviceArtifactSha256\":" +
           json_string(metadata.service_artifact_sha256) +
           ",\"serviceVersion\":" + json_string(metadata.service_version) +
           ",\"unitRole\":" + json_string(unit_role_name(metadata.unit_role)) +
           ",\"unitSystemUid\":" + json_string(metadata.unit_system_uid) +
           ",\"vdpContractSha256\":" + json_string(metadata.vdp_contract_sha256) +
           ",\"vdpContractVersion\":" + json_string(metadata.vdp_contract_version) + "}";
}

void validate_metadata(const MessageMetadata& metadata, const EventWindow& window) {
    if (!bounded_identifier(metadata.unit_system_uid) ||
        !semantic_version(metadata.service_version) ||
        !semantic_version(metadata.vdp_contract_version) ||
        !lowercase_sha256(metadata.service_artifact_sha256) ||
        !lowercase_sha256(metadata.vdp_contract_sha256) || !uuid4(window.event_id) ||
        !is_rfc3339_millisecond_utc(window.trigger_timestamp)) {
        throw std::invalid_argument("message metadata does not satisfy the v1 schema");
    }
    if (window.samples.empty() || window.samples.size() > 150U) {
        throw std::invalid_argument("window sample count does not satisfy the v1 schema");
    }
    const bool terminal_pair_valid =
        (window.terminal_state == TerminalState::Complete &&
         window.reason_code == ReasonCode::NormalClear) ||
        (window.terminal_state == TerminalState::TruncatedMaxDuration &&
         window.reason_code == ReasonCode::MaxActiveDuration) ||
        (window.terminal_state == TerminalState::IncompleteSourceGap &&
         window.reason_code == ReasonCode::SourceGap) ||
        (window.terminal_state == TerminalState::AbortedServiceStop &&
         window.reason_code == ReasonCode::ServiceStop) ||
        (window.terminal_state == TerminalState::AbortedRestart &&
         window.reason_code == ReasonCode::ServiceRestart);
    if (!terminal_pair_valid) {
        throw std::invalid_argument("terminal state and reason code do not satisfy the v1 schema");
    }
}

}  // namespace

std::size_t MessageSet::encoded_bytes() const {
    std::size_t result = completion.canonical_json.size();
    for (const CanonicalMessage& chunk : chunks) {
        result += chunk.canonical_json.size();
    }
    return result;
}

std::string canonicalize_chunk_content(
    std::size_t chunk_index,
    std::size_t first_sample_index,
    const std::vector<RetainedSample>& samples) {
    if (chunk_index > 14U || first_sample_index > 149U || samples.empty() ||
        samples.size() > 10U || first_sample_index + samples.size() > 150U) {
        throw std::invalid_argument("chunk bounds do not satisfy the v1 schema");
    }
    std::string output = "{\"chunkIndex\":" + std::to_string(chunk_index) +
                         ",\"firstSampleIndex\":" +
                         std::to_string(first_sample_index) +
                         ",\"sampleCount\":" + std::to_string(samples.size()) +
                         ",\"samples\":[";
    for (std::size_t index = 0; index < samples.size(); ++index) {
        if (index != 0U) {
            output.push_back(',');
        }
        output += sample_json(samples[index], first_sample_index + index);
    }
    output += "]}";
    return output;
}

std::string window_sha256(const std::vector<std::string>& chunk_content_sha256) {
    if (chunk_content_sha256.empty() || chunk_content_sha256.size() > 15U) {
        throw std::invalid_argument("window must contain one to fifteen chunk digests");
    }
    std::string raw;
    raw.reserve(chunk_content_sha256.size() * 32U);
    for (const std::string& value : chunk_content_sha256) {
        const Sha256Digest digest = hex_decode_sha256(value);
        raw.append(reinterpret_cast<const char*>(digest.data()), digest.size());
    }
    return sha256_hex(raw);
}

std::string canonicalize_completion_content(
    const EventWindow& window,
    const std::vector<std::string>& chunk_content_sha256) {
    if (window.samples.empty() || window.samples.size() > 150U ||
        !is_rfc3339_millisecond_utc(window.trigger_timestamp)) {
        throw std::invalid_argument("completion window is invalid");
    }
    const auto phase_count = [&window](Phase phase) {
        return static_cast<std::size_t>(std::count_if(
            window.samples.begin(), window.samples.end(), [phase](const RetainedSample& sample) {
                return sample.phase == phase;
            }));
    };
    const std::size_t pre = phase_count(Phase::Pre);
    const std::size_t active = phase_count(Phase::Active);
    const std::size_t post = phase_count(Phase::Post);
    if (pre > 30U || active > 100U || post > 20U ||
        chunk_content_sha256.size() < (window.samples.size() + 9U) / 10U ||
        chunk_content_sha256.size() > std::min<std::size_t>(15U, window.samples.size())) {
        throw std::invalid_argument("completion counts do not satisfy the v1 schema");
    }
    std::set<std::string> unique_hashes(
        chunk_content_sha256.begin(), chunk_content_sha256.end());
    if (unique_hashes.size() != chunk_content_sha256.size()) {
        throw std::invalid_argument("chunk content digests must be unique");
    }

    std::string hashes = "[";
    for (std::size_t index = 0; index < chunk_content_sha256.size(); ++index) {
        if (index != 0U) {
            hashes.push_back(',');
        }
        if (!lowercase_sha256(chunk_content_sha256[index])) {
            throw std::invalid_argument("invalid chunk content digest");
        }
        hashes += json_string(chunk_content_sha256[index]);
    }
    hashes.push_back(']');

    return std::string("{") +
           "\"chunkContentSha256\":" + hashes +
           ",\"phaseSampleCounts\":{\"ACTIVE\":" + std::to_string(active) +
           ",\"POST\":" + std::to_string(post) +
           ",\"PRE\":" + std::to_string(pre) + "}" +
           ",\"reasonCode\":" + json_string(reason_code_name(window.reason_code)) +
           ",\"terminalState\":" + json_string(terminal_state_name(window.terminal_state)) +
           ",\"totalChunks\":" + std::to_string(chunk_content_sha256.size()) +
           ",\"totalSamples\":" + std::to_string(window.samples.size()) +
           ",\"triggerTimestamp\":" + json_string(window.trigger_timestamp) +
           ",\"windowEndTimestamp\":" +
           json_string(window.samples.back().frame.source_timestamp) +
           ",\"windowSha256\":" + json_string(window_sha256(chunk_content_sha256)) +
           ",\"windowStartTimestamp\":" +
           json_string(window.samples.front().frame.source_timestamp) + "}";
}

void enforce_message_size(std::string_view message) {
    if (message.size() > maximum_message_bytes) {
        throw std::length_error("canonical message exceeds 64 KiB");
    }
}

namespace {
MessageSet build_partitioned_messages(const MessageMetadata& metadata, const EventWindow& window, bool seal_pre) {
    validate_metadata(metadata, window);
    const auto first_non_pre = std::find_if(window.samples.begin(), window.samples.end(),
        [](const RetainedSample& sample) { return sample.phase != Phase::Pre; });
    const auto pre_count = static_cast<std::size_t>(std::distance(window.samples.begin(), first_non_pre));
    if (seal_pre && std::any_of(first_non_pre, window.samples.end(),
        [](const RetainedSample& sample) { return sample.phase == Phase::Pre; })) {
        throw std::invalid_argument("PRE samples must precede ACTIVE/POST");
    }
    MessageSet result;
    std::vector<std::string> chunk_hashes;
    for (std::size_t first = 0; first < window.samples.size();) {
        const auto end = seal_pre && first < pre_count ? pre_count : window.samples.size();
        const std::size_t count = std::min<std::size_t>(10U, end - first);
        std::vector<RetainedSample> chunk_samples(
            window.samples.begin() + static_cast<std::ptrdiff_t>(first),
            window.samples.begin() + static_cast<std::ptrdiff_t>(first + count));
        const std::size_t chunk_index = result.chunks.size();
        const std::string content =
            canonicalize_chunk_content(chunk_index, first, chunk_samples);
        const std::string content_hash = sha256_hex(content);
        std::string message = top_level_message(
            metadata, window, "WINDOW_CHUNK", content, content_hash);
        enforce_message_size(message);
        std::ostringstream filename;
        filename << "chunk-" << std::setw(3) << std::setfill('0') << chunk_index << ".json";
        result.chunks.push_back({filename.str(), std::move(message), content_hash});
        chunk_hashes.push_back(content_hash);
        first += count;
    }

    const std::string completion_content =
        canonicalize_completion_content(window, chunk_hashes);
    const std::string completion_hash = sha256_hex(completion_content);
    std::string completion_message = top_level_message(
        metadata, window, "WINDOW_COMPLETION", completion_content, completion_hash);
    enforce_message_size(completion_message);
    result.completion = {
        "completion.json", std::move(completion_message), completion_hash};
    return result;
}
}  // namespace

MessageSet build_messages(const MessageMetadata& metadata, const EventWindow& window) {
    return build_partitioned_messages(metadata, window, false);
}

MessageSet build_growing_messages(const MessageMetadata& metadata, const EventWindow& window) {
    return build_partitioned_messages(metadata, window, true);
}

const char* unit_role_name(UnitRole role) {
    switch (role) {
        case UnitRole::Validation:
            return "VALIDATION";
        case UnitRole::Production:
            return "PRODUCTION";
    }
    throw std::invalid_argument("unknown Unit role");
}

}  // namespace brake_health::v1
