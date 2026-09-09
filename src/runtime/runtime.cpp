// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0
#include "brake_health/runtime/runtime.hpp"
#include "brake_health/runtime/json.hpp"
#include "brake_health/v1/sha256.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace brake_health::runtime {
namespace {
std::string message_filename(std::optional<std::size_t> chunk_index) {
    if (!chunk_index) return "completion.json";
    if (*chunk_index > 14U) throw std::invalid_argument("CHUNK_INDEX_INVALID");
    std::ostringstream name;
    name << "chunk-" << std::setfill('0') << std::setw(3) << *chunk_index << ".json";
    return name.str();
}
std::string durable_message(const std::filesystem::path& path) {
    auto bytes = read_file(path, 65536);
    if (v1::sha256_hex(bytes) != read_file(path.string() + ".sha256", 64))
        throw std::runtime_error("SPOOL_INTEGRITY_INVALID");
    return bytes;
}
bool sealed_capture_chunk(const std::string& bytes) {
    const auto json = parse_json(bytes);
    const auto& content = json.at("content");
    const auto count = content.at("sampleCount").integer();
    const auto& samples = std::get<Json::Array>(content.at("samples").value);
    if (count < 1 || count > 10 || static_cast<std::size_t>(count) != samples.size())
        throw std::runtime_error("SPOOL_CHUNK_INVALID");
    // PRE is frozen at trigger. Only the trailing partial ACTIVE/POST chunk
    // can still grow; never expose its current idempotency key to transport.
    return count == 10 || std::all_of(samples.begin(), samples.end(),
        [](const Json& sample) { return sample.at("phase").string() == "PRE"; });
}
void verify_completed_set(const std::filesystem::path& directory, const v1::SpoolEntry& entry) {
    const auto completion = parse_json(durable_message(directory / "completion.json"));
    const auto& content = completion.at("content");
    const auto& expected_hashes = std::get<Json::Array>(content.at("chunkContentSha256").value);
    if (content.at("totalChunks").integer() != static_cast<std::int64_t>(entry.chunk_count) ||
        expected_hashes.size() != entry.chunk_count) throw std::runtime_error("SPOOL_SET_INVALID");
    std::int64_t expected_sample = 0;
    std::vector<std::string> hashes;
    for (std::size_t i = 0; i < entry.chunk_count; ++i) {
        const auto chunk = parse_json(durable_message(directory / message_filename(i)));
        const auto& values = chunk.at("content");
        if (chunk.at("eventId").string() != entry.event_id ||
            values.at("chunkIndex").integer() != static_cast<std::int64_t>(i) ||
            values.at("firstSampleIndex").integer() != expected_sample ||
            expected_hashes[i].string() != chunk.at("contentSha256").string())
            throw std::runtime_error("SPOOL_SET_INVALID");
        for (const auto* key : {"eventId", "unitSystemUid", "unitRole", "serviceVersion",
                "serviceArtifactSha256", "vdpContractVersion", "vdpContractSha256"})
            if (chunk.at(key).string() != completion.at(key).string())
                throw std::runtime_error("SPOOL_SET_INVALID");
        expected_sample += values.at("sampleCount").integer();
        hashes.push_back(chunk.at("contentSha256").string());
    }
    if (content.at("totalSamples").integer() != expected_sample ||
        content.at("windowSha256").string() != v1::window_sha256(hashes))
        throw std::runtime_error("SPOOL_SET_INVALID");
}
bool date_time(const std::string& text) {
    // ACK schema accepts RFC3339, not only the millisecond-UTC source profile.
    if (text.size() < 20 || text.size() > 128 || text[4] != '-' || text[7] != '-' ||
        (text[10] != 'T' && text[10] != 't') || text[13] != ':' || text[16] != ':') return false;
    const auto number = [&](std::size_t start, std::size_t length) {
        int n = 0;
        for (std::size_t i = start; i < start + length; ++i) {
            if (text[i] < '0' || text[i] > '9') return -1;
            n = n * 10 + text[i] - '0';
        }
        return n;
    };
    const int year = number(0, 4), month = number(5, 2), day = number(8, 2);
    const int hour = number(11, 2), minute = number(14, 2), second = number(17, 2);
    constexpr int days[]{0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (year < 0 || month < 1 || month > 12 || day < 1 || hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 || second < 0 || second > 60) return false;
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    if (day > days[month] + (month == 2 && leap ? 1 : 0)) return false;
    std::size_t p = 19;
    if (text[p] == '.') {
        const auto start = ++p;
        while (p < text.size() && text[p] >= '0' && text[p] <= '9') ++p;
        if (p == start || p == text.size()) return false;
    }
    if (text[p] == 'Z' || text[p] == 'z') return p + 1 == text.size();
    if ((text[p] != '+' && text[p] != '-') || p + 6 != text.size() || text[p + 3] != ':') return false;
    const int zone_hour = number(p + 1, 2), zone_minute = number(p + 4, 2);
    return zone_hour >= 0 && zone_hour <= 23 && zone_minute >= 0 && zone_minute <= 59;
}
}  // namespace
std::string utc_timestamp(std::int64_t epoch_ms) {
    if (epoch_ms < 0) throw std::invalid_argument("TIME_INVALID");
    auto epoch = static_cast<std::time_t>(epoch_ms / 1000);
    std::tm tm{};
    if (!gmtime_r(&epoch, &tm)) throw std::invalid_argument("TIME_INVALID");
    char out[32];
    if (std::strftime(out, sizeof(out), "%Y-%m-%dT%H:%M:%S", &tm) == 0) throw std::invalid_argument("TIME_INVALID");
    std::ostringstream result; result << out << '.' << std::setfill('0') << std::setw(3) << epoch_ms % 1000 << 'Z';
    return result.str();
}
std::optional<v1::SourceFrame> complete_frame(const std::array<Signal, 6>& values,
    std::int64_t wall_ms, std::int64_t monotonic_ms, std::int64_t previous_epoch_ms) {
    const auto epoch = values[0].epoch_ms;
    // A complete VDP publication shares one source timestamp. Never assemble a
    // new frame from an old pedal sample and a newly arrived speed sample.
    if (epoch <= previous_epoch_ms) return std::nullopt;
    for (const auto& value : values) {
        if (!value.valid || !std::isfinite(value.value) || value.epoch_ms != epoch || wall_ms < epoch || wall_ms - epoch > 250) return std::nullopt;
    }
    if (std::floor(values[4].value) != values[4].value || values[4].value < 0 || values[4].value > 100 ||
        std::floor(values[5].value) != values[5].value || values[5].value < 0 || values[5].value > 100) return std::nullopt;
    return v1::SourceFrame{values[0].value, values[1].value, values[2].value, values[3].value,
        static_cast<int>(values[4].value), static_cast<int>(values[5].value), utc_timestamp(epoch), epoch,
        monotonic_ms, static_cast<int>(wall_ms - epoch), v1::FrameQuality::ValidCompleteFrame};
}
std::string credential_request(const std::string& secret) {
    if (secret.empty()) throw std::invalid_argument("AOS_SECRET_UNAVAILABLE");
    auto request = "{\"protocol\":\"aos-kuksa-auth-compat/v1\",\"operation\":\"issue\",\"aosSecret\":" + quote_json(secret) + "}\n";
    if (request.size() > 16384) throw std::invalid_argument("AOS_SECRET_INVALID");
    return request;
}
Credential parse_credential(const std::string& response, std::int64_t now) {
    if (response.empty() || response.back() != '\n' || response.size() > 32768) throw std::invalid_argument("KAC_RESPONSE_INVALID");
    const auto json = parse_json(response, 32768);
    if (json.at("protocol").string() != "aos-kuksa-auth-compat/v1" || json.at("correlationId").string().empty()) throw std::invalid_argument("KAC_RESPONSE_INVALID");
    Credential result;
    if (json.at("status").string() == "rejected") {
        if (json.object().size() != 5) throw std::invalid_argument("KAC_RESPONSE_INVALID");
        result.code = json.at("code").string(); result.retryable = json.at("retryable").boolean();
        const std::vector<std::string> retryable{"IAM_UNAVAILABLE", "SIGNER_UNAVAILABLE", "TIME_UNTRUSTED", "BUSY"};
        const std::vector<std::string> terminal{"INVALID_REQUEST", "DENIED", "POLICY_UNSUPPORTED", "INTERNAL_ERROR"};
        const bool retry = std::find(retryable.begin(), retryable.end(), result.code) != retryable.end();
        if (result.retryable != retry || (!retry && std::find(terminal.begin(), terminal.end(), result.code) == terminal.end())) throw std::invalid_argument("KAC_RESPONSE_INVALID");
        return result;
    }
    if (json.at("status").string() != "issued" || json.object().size() != 6) throw std::invalid_argument("KAC_RESPONSE_INVALID");
    result.token = json.at("token").string(); result.expires = json.at("expiresAtUnixSeconds").integer(); result.renew_after = json.at("renewAfterUnixSeconds").integer();
    if (result.token.empty() || result.token.size() > 16384 || result.token.front() == '.' || result.token.back() == '.' ||
        result.token.find("..") != std::string::npos || result.expires <= now || result.expires > now + 300 ||
        result.expires - result.renew_after != 120 || result.renew_after <= now ||
        std::count(result.token.begin(), result.token.end(), '.') != 2 ||
        !std::all_of(result.token.begin(), result.token.end(), [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'; })) throw std::invalid_argument("KAC_RESPONSE_INVALID");
    return result;
}
bool retryable_http(int status) {
    return status == 0 || status == 408 || status == 425 || status == 429 || status == 500 || status == 502 || status == 503 || status == 504;
}
int retry_delay(unsigned attempt, double jitter, int retry_after) {
    if (!std::isfinite(jitter) || jitter < -0.2 || jitter > 0.2 || retry_after < 0) throw std::invalid_argument("RETRY_INVALID");
    int base = std::min(30, 1 << std::min(attempt, 5U));
    return std::max(retry_after, std::clamp(static_cast<int>(std::ceil(base * (1.0 + jitter))), 1, 30));
}
bool matches_ack(const std::string& message, const HttpResponse& response) {
    if (response.status != 200 && response.status != 201) return false;
    try {
        const auto msg = parse_json(message), ack = parse_json(response.body, 8192);
        const auto kind = msg.at("messageType").string();
        const auto* identity = kind == "BRAKE_HEALTH_ASSESSMENT" ? "assessmentId" :
            kind == "BRAKE_ADVISORY_FACT" ? "requestId" : "eventId";
        if (kind != "WINDOW_CHUNK" && kind != "WINDOW_COMPLETION" && kind != "BRAKE_HEALTH_ASSESSMENT" &&
            kind != "BRAKE_HEALTH_EVENT" && kind != "BRAKE_ADVISORY_FACT") return false;
        auto key = '[' + quote_json(msg.at("unitSystemUid").string()) + ',' + quote_json(kind) + ',' + quote_json(msg.at(identity).string());
        if (kind == "WINDOW_CHUNK") {
            const auto index = msg.at("content").at("chunkIndex").integer();
            if (index < 0 || index > 14) return false;
            key += ',' + std::to_string(index);
        } else if (kind == "BRAKE_ADVISORY_FACT") {
            const auto state = msg.at("gatewayState").string();
            if (state != "RECEIVED" && state != "APPLIED" && state != "CLEARED" && state != "REJECTED" &&
                state != "EXPIRED" && state != "FAILED") return false;
            key += ',' + quote_json(state);
        }
        key += ']';
        if (!is_sha256(msg.at("contentSha256").string())) return false;
        return ack.object().size() == 7 && ack.at("schemaVersion").integer() == 1 && ack.at("contractVersion").string() == "1.0.0" &&
            is_uuid(ack.at("receiptId").string()) && date_time(ack.at("receivedAt").string()) &&
            (ack.at("state").string() == "DURABLE_ACCEPTED" || ack.at("state").string() == "DUPLICATE_ACCEPTED") &&
            ack.at("contentSha256").string() == msg.at("contentSha256").string() &&
            ack.at("messageKeySha256").string() == v1::sha256_hex(key);
    } catch (...) { return false; }
}
Runtime::Runtime(std::filesystem::path root, v1::MessageMetadata metadata, v1::UuidSource uuid)
    : root_(std::move(root)), metadata_(std::move(metadata)), engine_(std::move(uuid)), spool_(root_) {
    for (const auto& entry : spool_.recover()) {
        if (entry.state == v1::SpoolState::Quarantined) continue;
        // Individual files may all have valid hashes after a crash between
        // writes but belong to different capture checkpoints. Never send that
        // inconsistent set as a completed event.
        try { verify_completed_set(root_ / entry.event_id, entry); }
        catch (...) { spool_.quarantine(entry.event_id); }
    }
}
void Runtime::store(const v1::EventWindow& window, bool capturing) {
    if (dropped_event_id_ == window.event_id) return;
    const auto messages = v1::build_growing_messages(metadata_, window);
    const auto entries = spool_.inventory();
    const auto existing = std::find_if(entries.begin(), entries.end(),
        [&](const v1::SpoolEntry& entry) { return entry.event_id == window.event_id; });
    v1::AdmissionResult result;
    if (existing != entries.end()) {
        // A transport conflict freezes retained bytes. Continued acquisition or
        // shutdown must not overwrite the quarantined event or kill the process.
        if (existing->state == v1::SpoolState::Quarantined) return;
        try {
            for (std::size_t i = 0; i < existing->chunk_count; ++i) {
                const auto path = root_ / window.event_id / message_filename(i);
                const auto previous = durable_message(path);
                if (i >= messages.chunks.size() ||
                    ((sealed_capture_chunk(previous) || std::filesystem::exists(path.string() + ".ack")) &&
                     previous != messages.chunks[i].canonical_json))
                    throw std::runtime_error("SEALED_CHUNK_CHANGED");
            }
        } catch (...) {
            spool_.quarantine(window.event_id);
            return;
        }
        result = capturing ? spool_.checkpoint_capturing(window.event_id, messages)
                           : spool_.complete_capturing(window.event_id, messages);
    } else {
        result = capturing ? spool_.store_capturing_for_recovery(window.event_id, messages)
                           : spool_.store_completed(window.event_id, messages);
    }
    // Admission is decided once per event, never reconsidered after a backlog
    // drains (which would silently admit a partially captured rejected event).
    if (result == v1::AdmissionResult::WindowDroppedQueueFull) {
        dropped_event_id_ = window.event_id;
        if (existing != entries.end()) spool_.quarantine(window.event_id);
    }
}
v1::IngestResult Runtime::ingest(const v1::SourceFrame& frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = engine_.ingest(frame);
    if (result.completed) store(*result.completed);
    else if (result.event_active && (result.retained || result.event_started)) {
        auto snapshot = engine_; auto interrupted = snapshot.abort_restart();
        if (interrupted) store(*interrupted, true);
    }
    return result;
}
void Runtime::disconnect() {
    v1::SourceFrame missing; missing.quality = v1::FrameQuality::Incomplete;
    ingest(missing);
}
void Runtime::stop() { std::lock_guard<std::mutex> lock(mutex_); auto window = engine_.abort_service_stop(); if (window) store(*window); }
void Runtime::update_vdp_metadata(const v1::MessageMetadata& metadata) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (metadata.unit_system_uid != metadata_.unit_system_uid || metadata.unit_role != metadata_.unit_role ||
        metadata.service_version != metadata_.service_version || metadata.service_artifact_sha256 != metadata_.service_artifact_sha256)
        throw std::invalid_argument("IMMUTABLE_IDENTITY_CHANGED");
    if (metadata.vdp_contract_version != metadata_.vdp_contract_version || metadata.vdp_contract_sha256 != metadata_.vdp_contract_sha256) {
        v1::SourceFrame missing; missing.quality = v1::FrameQuality::Incomplete;
        const auto result = engine_.ingest(missing);
        if (result.completed) store(*result.completed);  // Previous provenance, never relabel queued data.
        metadata_ = metadata;
    }
}
std::vector<v1::SpoolEntry> Runtime::inventory() { std::lock_guard<std::mutex> lock(mutex_); return spool_.inventory(); }
std::optional<PendingMessage> Runtime::next_message() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& event : spool_.inventory()) {
        const bool capturing = event.state == v1::SpoolState::Capturing;
        if (!capturing && event.state != v1::SpoolState::ReadyToSend && event.state != v1::SpoolState::WaitingAck) continue;
        if (event.state == v1::SpoolState::ReadyToSend) spool_.mark_waiting_ack(event.event_id);
        for (std::size_t i = 0; i < event.chunk_count + (capturing ? 0U : 1U); ++i) {
            const auto index = i < event.chunk_count ? std::optional<std::size_t>{i} : std::nullopt;
            const auto path = root_ / event.event_id / message_filename(index);
            if (std::filesystem::exists(path.string() + ".ack")) continue;
            try {
                auto bytes = durable_message(path);
                if (capturing && !sealed_capture_chunk(bytes)) break;
                return PendingMessage{event.event_id, index, std::move(bytes)};
            } catch (...) {
                spool_.quarantine(event.event_id);
                break;
            }
        }
        spool_.delete_if_fully_acknowledged(event.event_id);
    }
    return std::nullopt;
}
bool Runtime::accept(const PendingMessage& message, const HttpResponse& response) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto entries = spool_.inventory();
    const auto entry = std::find_if(entries.begin(), entries.end(),
        [&](const v1::SpoolEntry& value) { return value.event_id == message.event_id; });
    if (entry == entries.end() || entry->state == v1::SpoolState::Quarantined) return false;
    try {
        if (durable_message(root_ / message.event_id / message_filename(message.chunk_index)) != message.bytes)
            throw std::runtime_error("IN_FLIGHT_MESSAGE_CHANGED");
    } catch (...) {
        spool_.quarantine(message.event_id);
        return false;
    }
    if (!matches_ack(message.bytes, response)) {
        if (response.status != 0 && !retryable_http(response.status)) {
            spool_.quarantine(message.event_id);
        }
        return false;
    }
    if (message.chunk_index) spool_.acknowledge_chunk(message.event_id, *message.chunk_index);
    else spool_.acknowledge_completion(message.event_id);
    spool_.delete_if_fully_acknowledged(message.event_id);
    return true;
}
}  // namespace brake_health::runtime
