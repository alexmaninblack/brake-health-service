// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0
#include "brake_health/runtime/runtime.hpp"
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
    CHECK(!runtime.next_message());
}
std::string ack(const std::string& bytes, std::string date = "2026-09-09T00:00:00Z", std::string state = "DURABLE_ACCEPTED") {
    const auto message = parse_json(bytes);
    const auto kind = message.at("messageType").string();
    auto key = '[' + quote_json(message.at("unitSystemUid").string()) + ',' + quote_json(kind) + ',' + quote_json(message.at("eventId").string());
    if (kind == "WINDOW_CHUNK") key += ',' + std::to_string(message.at("content").at("chunkIndex").integer());
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
    struct stat info{}; CHECK(::stat(path.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600);
    rejects([&] { read_file(path, 2); });
    const auto link = directory.path / "link";
    std::filesystem::create_symlink(path, link);
    rejects([&] { read_file(link, 64); });
    CHECK(::chmod(directory.path.c_str(), 0755) == 0);
    rejects([&] { atomic_private_file(path, "invalid"); });
}
}  // namespace
int main() {
    const std::pair<const char*, std::function<void()>> groups[]{
        {"strict JSON", json_bounds}, {"coherent frames", coherent_input}, {"KAC envelopes", kac_envelope},
        {"HTTP and retry bounds", http_and_retry}, {"durable ACK delivery", durable_delivery},
        {"restart, source gap and conflict", restart_disconnect_and_conflict}, {"private file replacement", private_file}};
    int failed = 0;
    for (const auto& group : groups) {
        try { group.second(); std::cout << "PASS: " << group.first << '\n'; }
        catch (const std::exception& error) { ++failed; std::cerr << "FAIL: " << group.first << ": " << error.what() << '\n'; }
    }
    return failed == 0 ? 0 : 1;
}
