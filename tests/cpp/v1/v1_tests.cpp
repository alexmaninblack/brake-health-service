// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0

#include "brake_health/v1/messages.hpp"
#include "brake_health/v1/model.hpp"
#include "brake_health/v1/sha256.hpp"
#include "brake_health/v1/spool.hpp"
#include "brake_health/v1/window.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using namespace brake_health::v1;

class TestFailure : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            throw TestFailure(std::string("check failed: ") + #condition + " at line " + \
                              std::to_string(__LINE__));                                  \
        }                                                                                 \
    } while (false)

template <typename Exception, typename Callable>
void check_throws(Callable&& callable) {
    bool threw = false;
    try {
        callable();
    } catch (const Exception&) {
        threw = true;
    }
    CHECK(threw);
}

std::string timestamp(std::int64_t milliseconds) {
    const std::int64_t total_seconds = milliseconds / 1000;
    const std::int64_t minute = total_seconds / 60;
    const std::int64_t second = total_seconds % 60;
    const std::int64_t fraction = milliseconds % 1000;
    std::ostringstream output;
    output << "2026-08-22T12:" << std::setw(2) << std::setfill('0') << minute << ':'
           << std::setw(2) << second << '.' << std::setw(3) << fraction << 'Z';
    return output.str();
}

SourceFrame frame(
    std::int64_t milliseconds,
    double speed = 42.0,
    int brake = 0,
    FrameQuality quality = FrameQuality::ValidCompleteFrame) {
    return {
        speed,
        -3.0,
        0.0,
        0.0,
        0,
        brake,
        timestamp(milliseconds),
        1787400000000LL + milliseconds,
        milliseconds,
        20,
        quality,
    };
}

std::string uuid_for(int value) {
    std::ostringstream output;
    output << "00000000-0000-4000-8000-" << std::setw(12) << std::setfill('0') << value;
    return output.str();
}

WindowEngine engine_with_uuid(int value = 1) {
    return WindowEngine([value]() { return uuid_for(value); });
}

IngestResult start_event(WindowEngine& engine, std::int64_t start = 0) {
    IngestResult result;
    for (std::int64_t offset : {0LL, 50LL, 100LL, 150LL, 199LL, 200LL}) {
        result = engine.ingest(frame(start + offset, 10.0, 50));
    }
    CHECK(result.event_started);
    CHECK(engine.capturing());
    return result;
}

MessageMetadata metadata() {
    return {
        "demo-unit-validation-001",
        UnitRole::Validation,
        "1.0.0",
        std::string(64U, '1'),
        "1.0.0",
        std::string(64U, '2'),
    };
}

EventWindow golden_window(TerminalState state = TerminalState::AbortedServiceStop) {
    SourceFrame first = frame(0, 42.0, 60);
    first.longitudinal_acceleration_mps2 = 0.1;
    SourceFrame second = frame(200, 41.0, 65);
    second.max_source_age_ms = 22;
    return {
        "4cba2d80-c04a-4d24-9f03-f4a85d56da13",
        second.source_timestamp,
        state,
        state == TerminalState::AbortedRestart ? ReasonCode::ServiceRestart
                                               : ReasonCode::ServiceStop,
        {{first, Phase::Pre}, {second, Phase::Active}},
    };
}

class TemporaryDirectory {
public:
    explicit TemporaryDirectory(std::string suffix) {
        static int sequence = 0;
        path_ = std::filesystem::temp_directory_path() /
                ("brake-health-v1-" + std::to_string(::getpid()) + "-" +
                 std::to_string(sequence++) + "-" + std::move(suffix));
        std::filesystem::create_directory(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

std::string shallow_valid_json(std::size_t size) {
    const std::string prefix = "{\"contentSha256\":\"" + std::string(64U, '0') + "\",\"p\":\"";
    const std::string suffix = "\"}";
    if (size < prefix.size() + suffix.size()) {
        throw std::invalid_argument("requested dummy message is too small");
    }
    return prefix + std::string(size - prefix.size() - suffix.size(), 'x') + suffix;
}

MessageSet sized_message_set(std::size_t each_message_bytes, std::size_t chunks = 1U) {
    MessageSet messages;
    for (std::size_t index = 0; index < chunks; ++index) {
        std::ostringstream filename;
        filename << "chunk-" << std::setw(3) << std::setfill('0') << index << ".json";
        messages.chunks.push_back(
            {filename.str(), shallow_valid_json(each_message_bytes), std::string(64U, '0')});
    }
    messages.completion = {
        "completion.json", shallow_valid_json(each_message_bytes), std::string(64U, '0')};
    return messages;
}

void test_sha256_known_answers() {
    CHECK(sha256_hex("") ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256_hex("abc") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256_hex(std::string(1000000U, 'a')) ==
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    CHECK(hex_encode(hex_decode_sha256(std::string(64U, 'a'))) == std::string(64U, 'a'));
    check_throws<std::invalid_argument>([] { hex_decode_sha256(std::string(64U, 'A')); });
}

void test_frame_validation_and_cadence() {
    for(const int rate:{20,30}) {
        WindowEngine sampled=engine_with_uuid();std::size_t count=0;
        for(int i=0;i<rate*10;++i)count+=sampled.ingest(frame(i*1000/rate)).retained?1U:0U;
        CHECK(count==100U);
    }
    FrameValidator validator;
    CHECK(validator.validate(frame(0), std::nullopt, std::nullopt).valid);

    SourceFrame value = frame(0);
    value.quality = FrameQuality::Incomplete;
    CHECK(validator.validate(value, std::nullopt, std::nullopt).error == FrameError::Incomplete);
    value = frame(0);
    value.max_source_age_ms = 5000;
    CHECK(validator.validate(value, std::nullopt, std::nullopt).valid);
    value.max_source_age_ms = 5001;
    CHECK(validator.validate(value, std::nullopt, std::nullopt).error == FrameError::Stale);
    value.max_source_age_ms = -1;
    CHECK(validator.validate(value, std::nullopt, std::nullopt).error == FrameError::Future);
    value = frame(0);
    value.speed_kph = std::numeric_limits<double>::quiet_NaN();
    CHECK(validator.validate(value, std::nullopt, std::nullopt).error == FrameError::NonFinite);
    value = frame(0);
    value.brake_pedal_percent = 101;
    CHECK(validator.validate(value, std::nullopt, std::nullopt).error == FrameError::OutOfRange);
    value = frame(0);
    value.source_timestamp = "not-a-time";
    CHECK(validator.validate(value, std::nullopt, std::nullopt).error == FrameError::Malformed);
    CHECK(validator.validate(frame(99), 1787400000100LL, 100).error == FrameError::Reordered);

    WindowEngine engine = engine_with_uuid();
    CHECK(engine.ingest(frame(0)).retained);
    CHECK(!engine.ingest(frame(33)).retained);
    CHECK(!engine.ingest(frame(50, 42.0, 0, FrameQuality::Incomplete)).retained);
    CHECK(!engine.ingest(frame(66)).retained);
    CHECK(engine.ingest(frame(100)).retained);
    for (int index = 4; index < 93; ++index) {
        engine.ingest(frame(index * 33LL));
    }
    CHECK(engine.pre_sample_count() == 30U);

    WindowEngine elapsed = engine_with_uuid();
    elapsed.ingest(frame(0));
    elapsed.ingest(frame(33));
    elapsed.ingest(frame(66));
    CHECK(elapsed.pre_sample_count() == 1U);
    elapsed.ingest(frame(4000));
    elapsed.ingest(frame(4033));
    elapsed.ingest(frame(4066));
    CHECK(elapsed.pre_sample_count() == 1U);
    CHECK(!elapsed.ingest(frame(4066)).retained); // Duplicate cannot refill the bucket.
    CHECK(!elapsed.ingest(frame(4099,42.0,0,FrameQuality::Incomplete)).retained);
    CHECK(elapsed.ingest(frame(4100)).retained);
    CHECK(elapsed.pre_sample_count()==2U); // Missing buckets remain gaps.
}

void test_trigger_boundaries_and_evidence_only_acceleration() {
    for (const auto& condition : std::vector<std::pair<double, int>>{{9.999, 50}, {10.0, 49}}) {
        WindowEngine engine = engine_with_uuid();
        for (std::int64_t time : {0LL, 100LL, 200LL, 300LL}) {
            SourceFrame value = frame(time, condition.first, condition.second);
            value.longitudinal_acceleration_mps2 = -100.0;
            CHECK(!engine.ingest(value).event_started);
        }
        CHECK(!engine.capturing());
    }

    for (const auto& condition : std::vector<std::pair<double, int>>{{10.0, 50}, {10.001, 51}}) {
        WindowEngine engine = engine_with_uuid();
        for (std::int64_t time : {0LL, 50LL, 100LL, 150LL, 199LL}) {
            CHECK(!engine.ingest(frame(time, condition.first, condition.second)).event_started);
        }
        CHECK(engine.ingest(frame(200, condition.first, condition.second)).event_started);
    }

    WindowEngine reset = engine_with_uuid();
    reset.ingest(frame(0, 10.0, 50));
    reset.ingest(frame(100, 10.0, 50));
    reset.ingest(frame(150, 10.0, 50, FrameQuality::Incomplete));
    CHECK(!reset.ingest(frame(250, 10.0, 50)).event_started);
    CHECK(!reset.ingest(frame(449, 10.0, 50)).event_started);
    CHECK(reset.ingest(frame(450, 10.0, 50)).event_started);
}

void test_clear_post_and_retrigger_boundaries() {
    WindowEngine exact = engine_with_uuid(2);
    start_event(exact);
    for (std::int64_t time : {300LL, 600LL, 800LL}) {
        exact.ingest(frame(time, 10.0, 10));
    }
    CHECK(exact.active_sample_count() > 0U);
    CHECK(exact.capturing());

    exact.ingest(frame(900, 10.0, 9));
    exact.ingest(frame(1399, 10.0, 9));
    CHECK(exact.capturing());
    exact.ingest(frame(1400, 10.0, 9));
    CHECK(exact.capturing());
    CHECK(!exact.ingest(frame(3399, 10.0, 9)).completed.has_value());
    const IngestResult complete = exact.ingest(frame(3400, 10.0, 9));
    CHECK(complete.completed.has_value());
    CHECK(complete.completed->terminal_state == TerminalState::Complete);

    WindowEngine speed_boundary = engine_with_uuid(3);
    start_event(speed_boundary);
    speed_boundary.ingest(frame(300, 0.5, 50));
    speed_boundary.ingest(frame(800, 0.5, 50));
    CHECK(speed_boundary.capturing());

    WindowEngine speed_above = engine_with_uuid(9);
    start_event(speed_above);
    speed_above.ingest(frame(300, 0.501, 50));
    speed_above.ingest(frame(800, 0.501, 50));
    CHECK(speed_above.capturing());

    WindowEngine speed_below = engine_with_uuid(10);
    start_event(speed_below);
    speed_below.ingest(frame(300, 0.499, 50));
    speed_below.ingest(frame(799, 0.499, 50));
    CHECK(speed_below.capturing());
    speed_below.ingest(frame(800, 0.499, 50));
    CHECK(!speed_below.ingest(frame(2799, 0.499, 50)).completed.has_value());
    CHECK(speed_below.ingest(frame(2800, 0.499, 50)).completed.has_value());

    WindowEngine retrigger = engine_with_uuid(4);
    start_event(retrigger);
    retrigger.ingest(frame(300, 10.0, 9));
    retrigger.ingest(frame(800, 10.0, 9));
    const std::size_t before = retrigger.total_sample_count();
    retrigger.ingest(frame(900, 10.0, 50));
    const IngestResult resumed = retrigger.ingest(frame(1100, 10.0, 50));
    CHECK(resumed.event_started);
    CHECK(retrigger.capturing());
    CHECK(retrigger.total_sample_count() >= before);
    const auto stopped = retrigger.abort_service_stop();
    CHECK(stopped && stopped->event_id == uuid_for(4));
}

void test_terminal_states_maximum_and_suppression() {
    WindowEngine gap = engine_with_uuid(5);
    start_event(gap);
    const IngestResult invalid = gap.ingest(frame(250, 42.0, 60, FrameQuality::Incomplete));
    CHECK(invalid.completed);
    CHECK(invalid.completed->terminal_state == TerminalState::IncompleteSourceGap);

    WindowEngine stopped = engine_with_uuid(6);
    start_event(stopped);
    CHECK(stopped.abort_service_stop()->terminal_state == TerminalState::AbortedServiceStop);

    WindowEngine restarted = engine_with_uuid(7);
    start_event(restarted);
    CHECK(restarted.abort_restart()->terminal_state == TerminalState::AbortedRestart);

    WindowEngine maximum = engine_with_uuid(8);
    start_event(maximum);
    IngestResult terminal;
    for (std::int64_t time = 233; time <= 10200; time += 33) {
        terminal = maximum.ingest(frame(time, 42.0, 60));
        if (terminal.completed) {
            break;
        }
    }
    CHECK(terminal.completed);
    CHECK(terminal.completed->terminal_state == TerminalState::TruncatedMaxDuration);
    CHECK(terminal.completed->samples.size() <= 150U);
    std::size_t active = 0;
    for (const RetainedSample& sample : terminal.completed->samples) {
        active += sample.phase == Phase::Active ? 1U : 0U;
    }
    CHECK(active <= 100U);
    CHECK(maximum.retrigger_suppressed());
    CHECK(!maximum.ingest(frame(10300, 42.0, 60)).event_started);
    maximum.ingest(frame(10400, 0.4, 0));
    maximum.ingest(frame(10900, 0.4, 0));
    CHECK(!maximum.retrigger_suppressed());

    WindowEngine total_duration = engine_with_uuid(11);
    for (std::int64_t time = 0; time < 3000; time += 33) {
        total_duration.ingest(frame(time));
    }
    total_duration.ingest(frame(3000, 10.0, 50));
    total_duration.ingest(frame(3199, 10.0, 50));
    CHECK(total_duration.ingest(frame(3200, 10.0, 50)).event_started);
    std::optional<EventWindow> total_terminal;
    std::int64_t decision_time = 0;
    for (std::int64_t cycle = 3300; !total_terminal && cycle < 20000; cycle += 900) {
        for (const auto& point : std::vector<std::pair<std::int64_t, int>>{
                 // One retained POST frame per cycle; otherwise the separate
                 // 20-POST-sample cap completes first under 100-ms retention.
                 {cycle, 9}, {cycle + 500, 9}, {cycle + 501, 50}, {cycle + 701, 50}}) {
            const IngestResult result = total_duration.ingest(frame(point.first, 10.0, point.second));
            if (result.completed) {
                total_terminal = result.completed;
                decision_time = point.first;
                break;
            }
        }
    }
    CHECK(total_terminal);
    CHECK(total_terminal->terminal_state == TerminalState::TruncatedMaxDuration);
    CHECK(total_terminal->samples.size() <= 150U);
    CHECK(decision_time - total_terminal->samples.front().frame.monotonic_ms >= 15000);
    CHECK(decision_time - total_terminal->samples.front().frame.monotonic_ms < 15900);
}

void test_golden_messages_and_bounds() {
    const MessageSet messages = build_messages(metadata(), golden_window());
    CHECK(messages.chunks.size() == 1U);
    CHECK(messages.chunks[0].content_sha256 ==
          "aab0a76d56fa55c2407fa52e1e84805b739ef0b1d45afc86d9804f60ebb2ab5d");
    CHECK(messages.completion.content_sha256 ==
          "42b0fe6447df1cf72e144b57af316d787dddab4e05579e4811b16f88de299f58");
    CHECK(sha256_hex(messages.chunks[0].canonical_json) ==
          "395b562e846decb720e80162b66d26b894978a498fef59e24824b1f312e45b8c");
    CHECK(sha256_hex(messages.completion.canonical_json) ==
          "e9b805fb355595e375b96a87dd5acafe0dbfac02d725226568287f06a232e289");
    CHECK(messages.completion.canonical_json.find(
              "\"windowSha256\":\"fa6d1798446835e3e082ba51ad535f989b703f84645544a18bdc0348df0195bc\"") !=
          std::string::npos);
    CHECK(messages.chunks[0].canonical_json.size() <= 65536U);
    CHECK(messages.completion.canonical_json.size() <= 65536U);

    EventWindow numeric = golden_window();
    numeric.samples[0].frame.speed_kph = 1e-6;
    numeric.samples[0].frame.longitudinal_acceleration_mps2 = 1e-7;
    const MessageSet numeric_messages = build_messages(metadata(), numeric);
    CHECK(numeric_messages.chunks[0].canonical_json.find("\"speedKph\":0.000001") !=
          std::string::npos);
    CHECK(numeric_messages.chunks[0].canonical_json.find(
              "\"longitudinalAccelerationMps2\":1e-7") != std::string::npos);

    enforce_message_size(std::string(65536U, 'x'));
    check_throws<std::length_error>([] { enforce_message_size(std::string(65537U, 'x')); });

    EventWindow many = golden_window();
    while (many.samples.size() < 11U) {
        SourceFrame value = frame(static_cast<std::int64_t>(many.samples.size()) * 100);
        many.samples.push_back({value, Phase::Active});
    }
    const MessageSet chunked = build_messages(metadata(), many);
    CHECK(chunked.chunks.size() == 2U);
    CHECK(chunked.chunks[0].filename == "chunk-000.json");
    CHECK(chunked.chunks[1].filename == "chunk-001.json");

    EventWindow invalid_pair = golden_window();
    invalid_pair.reason_code = ReasonCode::SourceGap;
    check_throws<std::invalid_argument>([&] { build_messages(metadata(), invalid_pair); });
}

void test_spool_window_and_byte_admission() {
    TemporaryDirectory window_root("window-limit");
    EventSpool windows(window_root.path());
    const MessageSet tiny = sized_message_set(128U);
    for (int index = 1; index <= 8; ++index) {
        CHECK(windows.store_completed(uuid_for(index), tiny) == AdmissionResult::Stored);
    }
    CHECK(windows.store_completed(uuid_for(9), tiny) ==
          AdmissionResult::WindowDroppedQueueFull);
    CHECK(windows.inventory().size() == 8U);
    CHECK(windows.dropped_queue_full() == 1U);

    TemporaryDirectory byte_root("byte-limit");
    EventSpool bytes(byte_root.path());
    const MessageSet one_mib = sized_message_set(65536U, 15U);
    CHECK(one_mib.encoded_bytes() == 1024U * 1024U);
    for (int index = 1; index <= 4; ++index) {
        CHECK(bytes.store_completed(uuid_for(index), one_mib) == AdmissionResult::Stored);
    }
    CHECK(bytes.inventory().size() == 4U);
    CHECK(bytes.store_completed(uuid_for(5), tiny) ==
          AdmissionResult::WindowDroppedQueueFull);
    CHECK(bytes.inventory().size() == 4U);
}

void test_spool_durability_faults_and_modes() {
    for (WriteStage failed_stage : {
             WriteStage::TemporaryOpen,
             WriteStage::TemporaryWrite,
             WriteStage::FileSync,
             WriteStage::Rename,
             WriteStage::DirectorySync,
         }) {
        TemporaryDirectory root("fault");
        bool injected = false;
        EventSpool spool(root.path(), [&](WriteStage stage, const std::filesystem::path& path) {
            const bool message_directory_sync =
                failed_stage != WriteStage::DirectorySync || path != root.path();
            if (!injected && stage == failed_stage && message_directory_sync) {
                injected = true;
                return true;
            }
            return false;
        });
        check_throws<std::runtime_error>([&] {
            spool.store_completed(uuid_for(1), sized_message_set(128U));
        });
        CHECK(injected);
    }

    TemporaryDirectory root("modes");
    std::vector<WriteStage> stages;
    EventSpool spool(root.path(), [&](WriteStage stage, const std::filesystem::path&) {
        stages.push_back(stage);
        return false;
    });
    CHECK(spool.store_completed(uuid_for(1), sized_message_set(128U)) ==
          AdmissionResult::Stored);
    CHECK(std::find(stages.begin(), stages.end(), WriteStage::TemporaryWrite) != stages.end());
    CHECK(std::find(stages.begin(), stages.end(), WriteStage::FileSync) != stages.end());
    CHECK(std::find(stages.begin(), stages.end(), WriteStage::Rename) != stages.end());
    CHECK(std::find(stages.begin(), stages.end(), WriteStage::DirectorySync) != stages.end());

    struct stat status {};
    CHECK(::stat(root.path().c_str(), &status) == 0);
    CHECK((status.st_mode & 0777) == 0700);
    const std::filesystem::path event = root.path() / uuid_for(1);
    CHECK(::stat(event.c_str(), &status) == 0);
    CHECK((status.st_mode & 0777) == 0700);
    CHECK(::stat((event / "manifest").c_str(), &status) == 0);
    CHECK((status.st_mode & 0777) == 0600);
    CHECK(::stat((event / "chunk-000.json").c_str(), &status) == 0);
    CHECK((status.st_mode & 0777) == 0600);
}

void test_spool_recovery_quarantine_and_ack_deletion() {
    TemporaryDirectory recovery_root("recovery");
    EventSpool recovery(recovery_root.path());
    MessageSet restart_messages = build_messages(
        metadata(), golden_window(TerminalState::AbortedRestart));
    CHECK(recovery.store_capturing_for_recovery(uuid_for(1), restart_messages) ==
          AdmissionResult::Stored);
    CHECK(recovery.checkpoint_capturing(uuid_for(1), restart_messages) ==
          AdmissionResult::Stored);
    const std::vector<SpoolEntry> recovered = recovery.recover();
    CHECK(recovered.size() == 1U);
    CHECK(recovered[0].state == SpoolState::ReadyToSend);
    CHECK(recovered[0].completion_present);
    CHECK(!std::filesystem::exists(
        recovery_root.path() / uuid_for(1) / "restart-completion.json"));

    TemporaryDirectory corrupt_root("corrupt");
    EventSpool corrupt(corrupt_root.path());
    CHECK(corrupt.store_completed(uuid_for(2), build_messages(metadata(), golden_window())) ==
          AdmissionResult::Stored);
    {
        std::ofstream stream(corrupt_root.path() / uuid_for(2) / "chunk-000.json");
        stream << "corrupt";
    }
    CHECK(corrupt.recover()[0].state == SpoolState::Quarantined);

    TemporaryDirectory ack_root("ack");
    EventSpool ack(ack_root.path());
    CHECK(ack.store_completed(uuid_for(3), build_messages(metadata(), golden_window())) ==
          AdmissionResult::Stored);
    ack.mark_waiting_ack(uuid_for(3));
    ack.acknowledge_chunk(uuid_for(3), 0U);
    CHECK(!ack.delete_if_fully_acknowledged(uuid_for(3)));
    CHECK(std::filesystem::exists(ack_root.path() / uuid_for(3)));
    ack.acknowledge_completion(uuid_for(3));
    CHECK(ack.delete_if_fully_acknowledged(uuid_for(3)));
    CHECK(!std::filesystem::exists(ack_root.path() / uuid_for(3)));
}

}  // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"SHA-256 known-answer vectors", test_sha256_known_answers},
        {"frame validation and retained cadence", test_frame_validation_and_cadence},
        {"trigger boundaries and evidence-only acceleration",
         test_trigger_boundaries_and_evidence_only_acceleration},
        {"clear, post and same-event retrigger boundaries", test_clear_post_and_retrigger_boundaries},
        {"terminal states, maximum and suppression", test_terminal_states_maximum_and_suppression},
        {"golden messages and size bounds", test_golden_messages_and_bounds},
        {"spool window and byte admission", test_spool_window_and_byte_admission},
        {"spool durability faults and modes", test_spool_durability_faults_and_modes},
        {"spool recovery, quarantine and acknowledgement deletion",
         test_spool_recovery_quarantine_and_ack_deletion},
    };

    std::size_t failures = 0;
    for (const auto& test : tests) {
        try {
            test.second();
            std::cout << "PASS: " << test.first << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL: " << test.first << ": " << error.what() << '\n';
        }
    }
    if (failures != 0U) {
        std::cerr << failures << " test group(s) failed\n";
        return 1;
    }
    std::cout << tests.size() << " deterministic Brake Health v1 test groups passed\n";
    return 0;
}
