// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "brake_health/v1/spool.hpp"
#include "brake_health/v1/window.hpp"

#include <array>
#include <atomic>
#include <filesystem>
#include <optional>
#include <mutex>
#include <string>
#include <vector>

namespace brake_health::runtime {
inline constexpr std::array<const char*, 6> paths = {
    "Vehicle.Speed", "Vehicle.Acceleration.Longitudinal", "Vehicle.Acceleration.Lateral",
    "Vehicle.Acceleration.Vertical", "Vehicle.Chassis.Accelerator.PedalPosition", "Vehicle.Chassis.Brake.PedalPosition"};
struct Signal {
    double value{};
    std::int64_t epoch_ms{};
    bool valid{};
};
std::optional<v1::SourceFrame> complete_frame(const std::array<Signal, 6>& signals,
    std::int64_t wall_ms, std::int64_t monotonic_ms, std::int64_t previous_epoch_ms);
std::string utc_timestamp(std::int64_t epoch_ms);
std::string random_uuid();
std::string read_file(const std::filesystem::path& path, std::size_t limit);
void atomic_private_file(const std::filesystem::path& path, const std::string& bytes);

struct Credential {
    std::string token;
    std::int64_t expires{};
    std::int64_t renew_after{};
    std::string code;
    bool retryable{};
};
Credential parse_credential(const std::string& response, std::int64_t now_seconds);
std::string credential_request(const std::string& secret);
std::string exchange_credential(const std::string& request, const std::atomic<bool>& stop);

struct HttpResponse { int status{}; std::string body; int retry_after{}; };
HttpResponse parse_http_response(const std::string& bytes);
HttpResponse post_backend(const std::string& bytes, const std::atomic<bool>& stop);
bool matches_ack(const std::string& message, const HttpResponse& response);
bool retryable_http(int status);
int retry_delay(unsigned attempt, double jitter, int retry_after = 0);

struct PendingMessage {
    std::string event_id;
    std::optional<std::size_t> chunk_index;
    std::string bytes;
};
// Exactly one analytics thread and one independent delivery thread share this
// queue. Network calls are made outside its lock and never block acquisition.
class Runtime {
public:
    Runtime(std::filesystem::path root, v1::MessageMetadata metadata,
            v1::UuidSource uuid = random_uuid);
    v1::IngestResult ingest(const v1::SourceFrame& frame);
    void disconnect();
    void stop();
    void update_vdp_metadata(const v1::MessageMetadata& metadata);
    std::optional<PendingMessage> next_message();
    bool accept(const PendingMessage& message, const HttpResponse& response);
    std::vector<v1::SpoolEntry> inventory();
private:
    void store(const v1::EventWindow& window, bool capturing = false);
    std::filesystem::path root_;
    v1::MessageMetadata metadata_;
    v1::WindowEngine engine_;
    v1::EventSpool spool_;
    std::optional<std::string> dropped_event_id_;
    std::mutex mutex_;
};
}  // namespace brake_health::runtime
