// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0
#include "brake_health/runtime/runtime.hpp"
#include "brake_health/runtime/derived_delivery.hpp"
#include "brake_health/runtime/json.hpp"
#include "brake_health/v1/sha256.hpp"

#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace {
using namespace brake_health::runtime;
namespace v1 = brake_health::v1;
namespace v2 = brake_health::v2;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(#x) + " at " + std::to_string(__LINE__)); } while (false)
template<class F> void rejects(F call) {
    bool rejected = false;
    try { call(); } catch (const std::exception&) { rejected = true; }
    CHECK(rejected);
}
struct Directory {
    std::filesystem::path path;
    Directory() {
        auto pattern = (std::filesystem::temp_directory_path() / "bhs-runtime-XXXXXX").string();
        auto made = ::mkdtemp(pattern.data());
        if (!made) throw std::runtime_error("mkdtemp");
        path = made;
    }
    ~Directory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};
const std::string event_id = "00000000-0000-4000-8000-000000000001";
v1::MessageMetadata metadata() {
    // Deterministic host-test input only; never a deployable runtime identity.
    return {"host-test-unit", v1::UnitRole::Validation, "1.0.0", std::string(64, '1'), "1.0.0", std::string(64, '2')};
}
v1::SourceFrame frame(std::int64_t time) {
    constexpr std::int64_t epoch = 1787400000000LL;
    return {42, -3, 0, 0, 0, 60, utc_timestamp(epoch + time), epoch + time, time, 20, v1::FrameQuality::ValidCompleteFrame};
}
void start(Runtime& runtime) {
    for (int time : {0, 50, 100, 150}) CHECK(!runtime.ingest(frame(time)).event_started);
    CHECK(runtime.ingest(frame(200)).event_started);
    CHECK(runtime.inventory().size() == 1);
    CHECK(runtime.inventory().front().state == v1::SpoolState::Capturing);
    const auto pre = runtime.next_message();
    CHECK(pre && pre->chunk_index == 0);
    CHECK(parse_json(pre->bytes).at("content").at("sampleCount").integer() == 1);
}
std::string ack(const std::string& bytes, std::string date = "2026-09-09T00:00:00Z", std::string state = "DURABLE_ACCEPTED") {
    const auto message = parse_json(bytes);
    const auto kind = message.at("messageType").string();
    const auto* identity = kind == "BRAKE_HEALTH_ASSESSMENT" ? "assessmentId" :
        kind == "BRAKE_ADVISORY_FACT" ? "requestId" : "eventId";
    auto key = '[' + quote_json(message.at("unitSystemUid").string()) + ',' + quote_json(kind) + ',' + quote_json(message.at(identity).string());
    if (kind == "WINDOW_CHUNK") key += ',' + std::to_string(message.at("content").at("chunkIndex").integer());
    if (kind == "BRAKE_ADVISORY_FACT") key += ',' + quote_json(message.at("gatewayState").string());
    key += ']';
    return "{\"schemaVersion\":1,\"contractVersion\":\"1.0.0\",\"receiptId\":\"00000000-0000-4000-8000-000000000002\",\"messageKeySha256\":" + quote_json(v1::sha256_hex(key)) +
        ",\"contentSha256\":" + quote_json(message.at("contentSha256").string()) + ",\"state\":" + quote_json(state) + ",\"receivedAt\":" + quote_json(date) + '}';
}
void json_bounds() {
    CHECK(parse_json("{\"a\":1,\"b\":true,\"s\":\"\\uD83D\\uDE97\"}").at("a").integer() == 1);
    CHECK(parse_json(quote_json("line\n\"quoted\"\\")).string() == "line\n\"quoted\"\\");
    CHECK(quote_json("\b\t\n\f\r") == "\"\\b\\t\\n\\f\\r\"");
    for (auto bad : {"{\"a\":1,\"a\":2}", "[1,]", "1 trailing", "01", "1e999", "9223372036854775808", "\"\\ud800\"", "{\"a\":NaN}"}) rejects([&] { parse_json(bad); });
    rejects([] { parse_json(std::string(25, '[') + "0" + std::string(25, ']')); });
    rejects([] { parse_json("12345", 4); });
    rejects([] { parse_json(std::string("\"\xc0\xaf\"", 4)); });
    rejects([] { quote_json(std::string("\xed\xa0\x80", 3)); });
    CHECK(is_uuid(random_uuid()));
    CHECK(is_uuid("AAAAAAAA-AAAA-4AAA-8AAA-AAAAAAAAAAAA"));
    CHECK(!is_uuid("../bad"));
}
void coherent_input() {
    std::array<Signal, 6> values{{{42, 1000, true}, {-3, 1000, true}, {0, 1000, true}, {0, 1000, true}, {0, 1000, true}, {60, 1000, true}}};
    CHECK(complete_frame(values, 1250, 100, 999));
    CHECK(!complete_frame(values, 1251, 100, 999));
    CHECK(!complete_frame(values, 999, 100, 999));
    CHECK(!complete_frame(values, 1000, 100, 1000));
    values[3].epoch_ms = 999;
    CHECK(!complete_frame(values, 1000, 100, 998));
    values[3].epoch_ms = 1000; values[3].valid = false;
    CHECK(!complete_frame(values, 1000, 100, 999));
    values[3].valid = true; values[5].value = 50.1;
    CHECK(!complete_frame(values, 1000, 100, 999));
    values[5].value = std::numeric_limits<double>::quiet_NaN();
    CHECK(!complete_frame(values, 1000, 100, 999));
    CHECK(utc_timestamp(0) == "1970-01-01T00:00:00.000Z");
}
void kac_envelope() {
    const auto request = credential_request("test-only\"\n");
    CHECK(request.back() == '\n');
    CHECK(parse_json(request).object().size() == 3);
    CHECK(parse_json(request).at("aosSecret").string() == "test-only\"\n");
    rejects([] { credential_request(""); });
    rejects([] { credential_request(std::string(16384, 'x')); });
    const std::string issued = "{\"protocol\":\"aos-kuksa-auth-compat/v1\",\"status\":\"issued\",\"correlationId\":\"host-test\",\"token\":\"e30.e30.c2ln\",\"expiresAtUnixSeconds\":1300,\"renewAfterUnixSeconds\":1180}\n";
    const auto credential = parse_credential(issued, 1000);
    CHECK(credential.expires == 1300 && credential.renew_after == 1180);
    CHECK(credential.code.empty());
    rejects([&] { parse_credential(issued, 1300); });
    rejects([&] { parse_credential(issued, 999); });
    rejects([&] { parse_credential(issued.substr(0, issued.size() - 1), 1000); });
    auto bad = issued; bad.replace(bad.find("e30.e30.c2ln"), 11, "a..b");
    rejects([&] { parse_credential(bad, 1000); });
    auto denied = "{\"protocol\":\"aos-kuksa-auth-compat/v1\",\"status\":\"rejected\",\"correlationId\":\"host-test\",\"code\":\"DENIED\",\"retryable\":false}\n";
    CHECK(parse_credential(denied, 1000).code == "DENIED");
    auto busy = std::string(denied); busy.replace(busy.find("DENIED"), 6, "BUSY");
    rejects([&] { parse_credential(busy, 1000); });
    busy.replace(busy.find("false"), 5, "true");
    CHECK(parse_credential(busy, 1000).retryable);
}
void http_and_retry() {
    CHECK(parse_http_response("HTTP/1.1 201 Created\r\nContent-Length: 2\r\n\r\n{}").body == "{}");
    CHECK(parse_http_response("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1\r\n{\r\n1\r\n}\r\n0\r\n\r\n").body == "{}");
    CHECK(parse_http_response("HTTP/1.1 503 Unavailable\r\nRetry-After: 60\r\nContent-Length: 0\r\n\r\n").retry_after == 60);
    for (auto bad : {"garbage", "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\n{}", "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 2\r\n\r\n{}", "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nTransfer-Encoding: chunked\r\n\r\n{}", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nff\r\n{}\r\n0\r\n\r\n"}) rejects([&] { parse_http_response(bad); });
    rejects([] { parse_http_response("HTTP/1.1 200 OK\r\n\r\n" + std::string(8193, 'x')); });
    CHECK(retry_delay(0, 0) == 1 && retry_delay(4, 0) == 16);
    CHECK(retry_delay(99, .2) == 30 && retry_delay(99, 0, 60) == 60);
    CHECK(retryable_http(0) && retryable_http(503) && !retryable_http(409));
}
void durable_delivery() {
    Directory directory;
    Runtime runtime(directory.path, metadata(), [] { return event_id; });
    start(runtime);
    runtime.stop();
    CHECK(runtime.inventory().front().state == v1::SpoolState::ReadyToSend);
    CHECK(!std::filesystem::exists(directory.path / event_id / "restart-completion.json"));
    auto message = runtime.next_message(); CHECK(message && message->chunk_index == 0);
    CHECK(!runtime.accept(*message, {503, "", 0}));
    CHECK(runtime.next_message()->bytes == message->bytes);
    CHECK(!runtime.accept(*message, {0, "", 0}));
    CHECK(runtime.accept(*message, {201, ack(message->bytes), 0}));
    CHECK(std::filesystem::exists(directory.path / event_id));
    message = runtime.next_message(); CHECK(message && !message->chunk_index);
    CHECK(matches_ack(message->bytes, {200, ack(message->bytes, "2026-09-09T09:00:00.123456+09:00", "DUPLICATE_ACCEPTED"), 0}));
    CHECK(!matches_ack(message->bytes, {200, ack(message->bytes, "2026-02-30T00:00:00Z"), 0}));
    CHECK(runtime.accept(*message, {201, ack(message->bytes), 0}));
    CHECK(runtime.inventory().empty());
    CHECK(!runtime.next_message());
}
void restart_disconnect_and_conflict() {
    Directory restart;
    {
        Runtime runtime(restart.path, metadata(), [] { return event_id; });
        start(runtime);
    }
    Runtime recovered(restart.path, metadata(), [] { return event_id; });
    CHECK(recovered.inventory().front().state == v1::SpoolState::ReadyToSend);
    CHECK(read_file(restart.path / event_id / "completion.json", 65536).find("ABORTED_RESTART") != std::string::npos);
    auto message = recovered.next_message(); CHECK(message);
    CHECK(!recovered.accept(*message, {409, "{}", 0}));
    CHECK(recovered.inventory().front().state == v1::SpoolState::Quarantined);
    CHECK(!recovered.next_message());
    CHECK(std::filesystem::exists(restart.path / event_id));
    Directory disconnect;
    Runtime runtime(disconnect.path, metadata(), [] { return event_id; });
    start(runtime); runtime.disconnect();
    CHECK(read_file(disconnect.path / event_id / "completion.json", 65536).find("INCOMPLETE_SOURCE_GAP") != std::string::npos);
    message = runtime.next_message(); CHECK(message);
    auto mismatched = ack(message->bytes);
    auto offset = mismatched.find("messageKeySha256\":\"") + 19;
    mismatched[offset] = mismatched[offset] == '0' ? '1' : '0';
    CHECK(!runtime.accept(*message, {201, mismatched, 0}));
    CHECK(std::filesystem::exists(disconnect.path / event_id));
}
void private_file() {
    Directory directory;
    const auto path = directory.path / "credential";
    atomic_private_file(path, "host-test-data");
    CHECK(read_file(path, 64) == "host-test-data");
    atomic_private_file(path, "replacement");
    CHECK(read_file(path, 64) == "replacement");
    struct stat info{}; CHECK(::stat(path.c_str(), &info) == 0 && (info.st_mode & 0777) == 0400);
    rejects([&] { read_file(path, 2); });
    const auto link = directory.path / "link";
    std::filesystem::create_symlink(path, link);
    rejects([&] { read_file(link, 64); });
    CHECK(::chmod(directory.path.c_str(), 0755) == 0);
    rejects([&] { atomic_private_file(path, "invalid"); });
}
void vdp_change_keeps_provenance() {
    Directory directory;
    Runtime runtime(directory.path, metadata(), [] { return event_id; });
    start(runtime);
    auto next_metadata = metadata();
    next_metadata.vdp_contract_sha256 = std::string(64, '3');
    runtime.update_vdp_metadata(next_metadata);
    const auto message = runtime.next_message(); CHECK(message);
    CHECK(parse_json(message->bytes).at("vdpContractSha256").string() == metadata().vdp_contract_sha256);
    CHECK(read_file(directory.path / event_id / "completion.json", 65536).find("INCOMPLETE_SOURCE_GAP") != std::string::npos);
    next_metadata.service_artifact_sha256 = std::string(64, '4');
    rejects([&] { runtime.update_vdp_metadata(next_metadata); });
}
void growing_pre_active_and_completion() {
    Directory directory;
    Runtime runtime(directory.path, metadata(), [] { return event_id; });
    start(runtime);  // A short PRE is eligible immediately, while braking.
    const auto pre = *runtime.next_message();
    CHECK(pre.bytes.find("\"phase\":\"PRE\"") != std::string::npos);
    CHECK(!runtime.accept(pre, {503, "", 0}));
    for (int time = 250; time <= 1700; time += 50) runtime.ingest(frame(time));
    CHECK(runtime.next_message()->bytes == pre.bytes);
    CHECK(runtime.accept(pre, {201, ack(pre.bytes), 0}));
    const auto active = *runtime.next_message();
    CHECK(active.chunk_index == 1);
    const auto content = parse_json(active.bytes).at("content");
    CHECK(content.at("sampleCount").integer() == 10);
    CHECK(content.at("firstSampleIndex").integer() == 1);
    CHECK(active.bytes.find("\"phase\":\"ACTIVE\"") != std::string::npos);
    CHECK(runtime.inventory().front().state == v1::SpoolState::Capturing);
    for (int time = 1750; time <= 1850; time += 50) runtime.ingest(frame(time));
    CHECK(runtime.next_message()->bytes == active.bytes); // In-flight prefix stays sealed.
    CHECK(runtime.accept(active, {200, ack(active.bytes, "2026-09-09T00:00:00Z", "DUPLICATE_ACCEPTED"), 0}));
    CHECK(!runtime.next_message()); // Partial ACTIVE tail and restart completion are private.
    runtime.stop();
    const auto tail = *runtime.next_message();
    CHECK(tail.chunk_index == 2);
    CHECK(parse_json(tail.bytes).at("content").at("sampleCount").integer() == 1);
    CHECK(runtime.accept(tail, {201, ack(tail.bytes), 0}));
    const auto completion = *runtime.next_message();
    CHECK(!completion.chunk_index);
    const auto terminal = parse_json(completion.bytes).at("content");
    CHECK(terminal.at("totalChunks").integer() == 3 && terminal.at("totalSamples").integer() == 12);
    const auto& hashes = std::get<Json::Array>(terminal.at("chunkContentSha256").value);
    CHECK(hashes[0].string() == parse_json(pre.bytes).at("contentSha256").string());
    CHECK(hashes[1].string() == parse_json(active.bytes).at("contentSha256").string());
    CHECK(runtime.accept(completion, {201, ack(completion.bytes), 0}));
    CHECK(runtime.inventory().empty());
}
void growing_restart_and_quarantine() {
    Directory directory;
    {
        Runtime runtime(directory.path, metadata(), [] { return event_id; });
        start(runtime);
        auto pre = *runtime.next_message();
        CHECK(runtime.accept(pre, {201, ack(pre.bytes), 0}));
        runtime.ingest(frame(250)); // One mutable ACTIVE sample, never sent.
        CHECK(!runtime.next_message());
    }
    Runtime recovered(directory.path, metadata(), [] { return event_id; });
    auto pending = *recovered.next_message();
    CHECK(pending.chunk_index == 1); // ACKed PRE is not retransmitted after restart.
    CHECK(recovered.accept(pending, {201, ack(pending.bytes), 0}));
    pending = *recovered.next_message(); CHECK(!pending.chunk_index);
    CHECK(pending.bytes.find("ABORTED_RESTART") != std::string::npos);
    CHECK(recovered.accept(pending, {201, ack(pending.bytes), 0}));
    CHECK(recovered.inventory().empty());

    Directory conflict;
    Runtime runtime(conflict.path, metadata(), [] { return event_id; });
    start(runtime);
    const auto pre = *runtime.next_message();
    const auto checkpoint = read_file(conflict.path / event_id / "restart-completion.json", 65536);
    CHECK(!runtime.accept(pre, {409, "{}", 0}));
    runtime.ingest(frame(250)); runtime.disconnect(); runtime.stop();
    CHECK(runtime.inventory().front().state == v1::SpoolState::Quarantined);
    CHECK(read_file(conflict.path / event_id / "restart-completion.json", 65536) == checkpoint);
    CHECK(read_file(conflict.path / event_id / "chunk-000.json", 65536) == pre.bytes);
    CHECK(!runtime.accept(pre, {201, ack(pre.bytes), 0}));
    CHECK(!runtime.next_message());
}
void growing_interrupted_checkpoint() {
    Directory directory;
    {
        Runtime runtime(directory.path, metadata(), [] { return event_id; });
        start(runtime);
        const auto checkpoint_path = directory.path / event_id / "restart-completion.json";
        const auto previous = read_file(checkpoint_path, 65536);
        const auto pre = *runtime.next_message();
        CHECK(runtime.accept(pre, {201, ack(pre.bytes), 0}));
        runtime.ingest(frame(250));
        // Emulate a crash after the new chunk is durable, before publishing
        // the new checkpoint. Both old completion files remain hash-valid.
        atomic_private_file(checkpoint_path, previous);
        atomic_private_file(checkpoint_path.string() + ".sha256", v1::sha256_hex(previous));
    }
    Runtime recovered(directory.path, metadata(), [] { return event_id; });
    CHECK(recovered.inventory().front().state == v1::SpoolState::Quarantined);
    CHECK(!recovered.next_message());
    CHECK(std::filesystem::exists(directory.path / event_id / "chunk-000.json.ack"));
    CHECK(std::filesystem::exists(directory.path / event_id / "chunk-001.json"));
}
void growing_partition_bounds() {
    for (std::size_t pre_count = 0; pre_count <= 30; ++pre_count) {
        v1::EventWindow window{event_id, utc_timestamp(1787400000200LL), v1::TerminalState::Complete,
                               v1::ReasonCode::NormalClear, {}};
        for (std::size_t i = 0; i < pre_count + 120; ++i)
            window.samples.push_back({frame(static_cast<std::int64_t>(i) * 100),
                i < pre_count ? v1::Phase::Pre : i < pre_count + 100 ? v1::Phase::Active : v1::Phase::Post});
        const auto messages = v1::build_growing_messages(metadata(), window);
        CHECK(messages.chunks.size() == (pre_count + 9) / 10 + 12);
        CHECK(messages.chunks.size() <= 15);
        std::size_t expected = 0;
        for (const auto& chunk : messages.chunks) {
            const auto content = parse_json(chunk.canonical_json).at("content");
            CHECK(static_cast<std::size_t>(content.at("firstSampleIndex").integer()) == expected);
            expected += static_cast<std::size_t>(content.at("sampleCount").integer());
        }
        CHECK(expected == window.samples.size());
    }
}
void rejected_capture_is_not_readmitted() {
    Directory directory;
    unsigned event = 0;
    Runtime runtime(directory.path, metadata(), [&] {
        return std::string("00000000-0000-4000-8000-00000000000") + std::to_string(++event);
    });
    for (int i = 0; i < 8; ++i) {
        for (int offset = 0; offset <= 200; offset += 50) runtime.ingest(frame(i * 500 + offset));
        runtime.stop();
    }
    CHECK(runtime.inventory().size() == 8);
    for (int time = 4000; time <= 4200; time += 50) runtime.ingest(frame(time));
    CHECK(event == 9 && runtime.inventory().size() == 8);
    while (runtime.inventory().size() == 8) {
        const auto pending = *runtime.next_message();
        CHECK(runtime.accept(pending, {201, ack(pending.bytes), 0}));
    }
    for (int time = 4250; time <= 4450; time += 50) runtime.ingest(frame(time));
    runtime.stop();
    CHECK(runtime.inventory().size() == 7);
    CHECK(!std::filesystem::exists(directory.path / "00000000-0000-4000-8000-000000000009"));
}
v2::CompletedEpisode derived_episode(std::string id = event_id) {
    // Explicit host fixture; the application cannot source product records here.
    v2::CompletedEpisode episode{std::move(id), v2::TerminalState::Complete, 10, {}};
    for (unsigned i = 0; i < 7; ++i) {
        v2::Sample sample;
        sample.sample_index = i;
        sample.source_timestamp = utc_timestamp(1787400000000LL + i * 100);
        sample.phase = i == 0 ? v2::Phase::Pre : i == 6 ? v2::Phase::Post : v2::Phase::Active;
        sample.max_source_age_ms = 20;
        sample.signals = {42000, -8000, 100000, 0, {400000, 800000, 800000, 800000}, {10000, 40000, 40000, 40000}};
        episode.samples.push_back(sample);
    }
    return episode;
}
v2::DeploymentMetadata derived_metadata() {
    const auto source = metadata();
    return {source.unit_system_uid, v2::UnitRole::Validation, "2.0.0", source.service_artifact_sha256,
            "2.0.0", source.vdp_contract_sha256, source.service_artifact_sha256, v2::kModelConfigSha256,
            utc_timestamp(1787400001000LL)};
}
void derived_ack_and_retention() {
    Directory directory;
    const std::string epoch = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
    std::string retained, state;
    {
        v2::StateStore store(directory.path / "state", directory.path / "outbox", epoch);
        CHECK(store.process(derived_episode(), derived_metadata()).status == v2::ProcessStatus::Produced);
        CHECK(store.inventory().size() == 2);
        const auto message = *next_derived_message(store);
        CHECK(message.message_type == "BRAKE_HEALTH_ASSESSMENT");
        CHECK(!accept_derived_message(store, message, {503, "", 0}));
        CHECK(!accept_derived_message(store, message, {0, "", 0}));
        CHECK(next_derived_message(store)->canonical_json == message.canonical_json);
        state = v2::state_json(store.state());
        CHECK(accept_derived_message(store, message, {201, ack(message.canonical_json), 0}));
        CHECK(v2::state_json(store.state()) == state);
        retained = next_derived_message(store)->canonical_json;
        CHECK(store.inventory().size() == 1);
    }
    v2::StateStore recovered(directory.path / "state", directory.path / "outbox", epoch);
    CHECK(recovered.ready() && v2::state_json(recovered.state()) == state);
    const auto message = *next_derived_message(recovered);
    CHECK(message.message_type == "BRAKE_HEALTH_EVENT" && message.canonical_json == retained);
    CHECK(accept_derived_message(recovered, message, {200, ack(message.canonical_json,
        "2026-09-09T00:00:00Z", "DUPLICATE_ACCEPTED"), 0}));
    CHECK(recovered.inventory().empty());
    CHECK(v2::state_json(recovered.state()) == state);
}
void derived_conflict_retains_pair() {
    for (const int status : {200, 409, 422}) {
        Directory directory;
        const std::string epoch = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
        v2::StateStore store(directory.path / "state", directory.path / "outbox", epoch);
        CHECK(store.process(derived_episode(), derived_metadata()).status == v2::ProcessStatus::Produced);
        const auto message = *next_derived_message(store);
        const auto before = v2::state_json(store.state());
        CHECK(!accept_derived_message(store, message, {status, "{}", 0}));
        CHECK(store.inventory().size() == 2);
        for (const auto& entry : store.inventory()) CHECK(entry.quarantined);
        CHECK(!next_derived_message(store));
        CHECK(v2::state_json(store.state()) == before);
        CHECK(!accept_derived_message(store, message, {201, ack(message.canonical_json), 0}));
        v2::StateStore recovered(directory.path / "state", directory.path / "outbox", epoch);
        CHECK(recovered.ready() && !next_derived_message(recovered));
        CHECK(recovered.process(derived_episode("00000000-0000-4000-8000-000000000003"),
            derived_metadata()).status == v2::ProcessStatus::Produced);
        CHECK(next_derived_message(recovered)); // A delivery conflict does not disable future analytics.
    }
}
void advisory_ack_is_not_application_evidence() {
    // ACK-key projection fixture, not a complete or publishable advisory fact.
    const auto message = "{\"messageType\":\"BRAKE_ADVISORY_FACT\",\"unitSystemUid\":\"host-test-unit\","
        "\"requestId\":\"16223957-cdc3-57d4-af7d-74f78016ca0e\",\"gatewayState\":\"APPLIED\",\"contentSha256\":" +
        quote_json(std::string(64, '1')) + '}';
    const auto receipt = ack(message);
    CHECK(matches_ack(message, {201, receipt, 0}));
    auto different_status = message;
    different_status.replace(different_status.find("APPLIED"), 7, "EXPIRED");
    CHECK(!matches_ack(different_status, {201, receipt, 0}));
    CHECK(matches_ack(different_status, {201, ack(different_status), 0}));
    auto different_unit = message;
    different_unit.replace(different_unit.find("host-test-unit"), 14, "another-unit");
    CHECK(!matches_ack(different_unit, {201, receipt, 0}));
    auto different_content = message;
    different_content.replace(different_content.find(std::string(64, '1')), 64, std::string(64, '2'));
    CHECK(!matches_ack(different_content, {201, receipt, 0}));
    CHECK(!matches_ack(message, {202, receipt, 0}));
}
}  // namespace
int main() {
    const std::pair<const char*, std::function<void()>> groups[]{
        {"strict JSON", json_bounds}, {"coherent frames", coherent_input}, {"KAC envelopes", kac_envelope},
        {"HTTP and retry bounds", http_and_retry}, {"durable ACK delivery", durable_delivery},
        {"restart, source gap and conflict", restart_disconnect_and_conflict}, {"private file replacement", private_file},
        {"VDP change preserves captured provenance", vdp_change_keeps_provenance},
        {"growing PRE, ACTIVE and single completion", growing_pre_active_and_completion},
        {"growing restart ACK and conflict retention", growing_restart_and_quarantine},
        {"interrupted checkpoint is quarantined", growing_interrupted_checkpoint},
        {"rejected capture is never partly readmitted", rejected_capture_is_not_readmitted},
        {"derived exact ACK, retry and restart retention", derived_ack_and_retention},
        {"derived conflict retains atomic pair", derived_conflict_retains_pair},
        {"advisory ACK binds exact Gateway state", advisory_ack_is_not_application_evidence},
        {"growing partition bounds", growing_partition_bounds}};
    int failed = 0;
    for (const auto& group : groups) {
        try { group.second(); std::cout << "PASS: " << group.first << '\n'; }
        catch (const std::exception& error) { ++failed; std::cerr << "FAIL: " << group.first << ": " << error.what() << '\n'; }
    }
    return failed == 0 ? 0 : 1;
}
