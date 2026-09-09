// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0
#include "brake_health/runtime/application.hpp"
#include "brake_health/runtime/json.hpp"
#include "kuksa/val/v1/val.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <thread>

namespace {
using namespace brake_health::runtime;
namespace val = kuksa::val::v1;
static_assert(std::atomic<bool>::is_always_lock_free, "Signal flag must be lock-free");
std::atomic<bool> interrupted{false};
void signal_handler(int) { interrupted.store(true, std::memory_order_relaxed); }
class Log {
    std::mutex mutex_;
    std::map<std::string, std::string> previous_;
    std::int64_t minute_{};
    unsigned emitted_{}, suppressed_{};
public:
    void state(const std::string& event, const std::string& state, const std::string& reason, const std::string& source_event = "") {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto key = state + ':' + reason;
        if (event != "WINDOW_TRIGGERED" && event != "WINDOW_COMPLETED" && previous_[event] == key) return;
        const auto minute = boot_milliseconds() / 60000;
        if (minute != minute_) { minute_ = minute; emitted_ = 0; }
        if (emitted_ >= 60) { ++suppressed_; return; }
        previous_[event] = key; ++emitted_;
        std::cout << "{\"schemaVersion\":1,\"eventType\":" << quote_json(event)
                  << ",\"severity\":\"INFO\",\"observedAt\":" << quote_json(utc_timestamp(wall_milliseconds()))
                  << ",\"currentState\":" << quote_json(state) << ",\"reasonCode\":" << quote_json(reason)
                  << ",\"count\":" << 1 + suppressed_;
        if (!source_event.empty()) std::cout << ",\"sourceEventId\":" << quote_json(source_event);
        std::cout << '}' << std::endl;
        suppressed_ = 0;
    }
};
void pause(const std::atomic<bool>& stop, std::int64_t milliseconds) {
    const auto end = boot_milliseconds() + milliseconds;
    while (!stop && !interrupted && boot_milliseconds() < end) std::this_thread::sleep_for(std::chrono::milliseconds(50));
}
std::size_t path_index(const std::string& path) {
    const auto position = std::find(paths.begin(), paths.end(), path);
    if (position == paths.end()) throw std::runtime_error("KUKSA_UNREQUESTED_PATH");
    return static_cast<std::size_t>(position - paths.begin());
}
void verify_metadata(const val::GetResponse& response) {
    if (response.has_error() || response.errors_size() || response.entries_size() != 6) throw std::runtime_error("VDP_INCOMPATIBLE");
    std::set<std::size_t> seen;
    for (const auto& entry : response.entries()) {
        const auto index = path_index(entry.path());
        const auto& metadata = entry.metadata();
        const std::string unit = index == 0 ? "km/h" : index < 4 ? "m/s^2" : "percent";
        if (!seen.insert(index).second || !entry.has_metadata() || metadata.entry_type() != val::ENTRY_TYPE_SENSOR ||
            metadata.data_type() != (index < 4 ? val::DATA_TYPE_FLOAT : val::DATA_TYPE_UINT8) ||
            !metadata.has_unit() || metadata.unit() != unit) throw std::runtime_error("VDP_INCOMPATIBLE");
    }
}
Signal signal(const val::DataEntry& entry, std::size_t index) {
    const auto& value = entry.value();
    if (!entry.has_value() || !value.has_timestamp() || value.timestamp().seconds() < 0 ||
        value.timestamp().seconds() > 253402300799LL || value.timestamp().nanos() < 0 || value.timestamp().nanos() >= 1000000000) return {};
    // Protobuf represents VSS uint8 as uint32; no numeric coercions are accepted.
    if ((index < 4 && value.value_case() != val::Datapoint::kFloat) ||
        (index >= 4 && value.value_case() != val::Datapoint::kUint32)) return {};
    return {index < 4 ? static_cast<double>(value.float_()) : static_cast<double>(value.uint32()),
            value.timestamp().seconds() * 1000 + value.timestamp().nanos() / 1000000, true};
}
void deliver(Runtime& runtime, std::atomic<bool>& stop, Log& log) {
    std::mt19937 random(std::random_device{}());
    std::uniform_real_distribution<double> jitter(-0.2, 0.2);
    unsigned attempt = 0;
    while (!stop && !interrupted) {
        try {
            const auto pending = runtime.next_message();
            if (!pending) { attempt = 0; pause(stop, 100); continue; }
            HttpResponse response;
            try { response = post_backend(pending->bytes, stop); } catch (...) {}
            if (runtime.accept(*pending, response)) {
                attempt = 0; log.state("BACKEND_SYNC_CHANGED", "CONNECTED", "NONE");
            } else {
                log.state("BACKEND_SYNC_CHANGED", "BACKLOG", "NONE");
                pause(stop, retry_delay(attempt++, jitter(random), response.retry_after) * 1000LL);
            }
        } catch (...) {
            log.state("READINESS_CHANGED", "NOT_READY", "STORAGE_UNAVAILABLE");
            pause(stop, 1000);
        }
    }
}
// One context per session, watched independently of blocking gRPC calls.
// Token loss/change cancels the subscription, including a stalled Get/Read.
void subscribe(Runtime& runtime, const ApplicationInputs& inputs, std::atomic<bool>& stop, Log& log) {
    const auto metadata_bytes = read_file(inputs.metadata_file, 8192);
    runtime.update_vdp_metadata(parse_metadata(metadata_bytes));
    const auto ca = read_file(inputs.ca_file, 65536);
    const auto token = read_private_token(token_path);
    grpc::SslCredentialsOptions tls; tls.pem_root_certs = ca;
    grpc::ChannelArguments arguments;
    arguments.SetMaxReceiveMessageSize(65536);
    auto channel = grpc::CreateCustomChannel("Server:55555", grpc::SslCredentials(tls), arguments);
    auto stub = val::VAL::NewStub(channel);
    std::mutex context_mutex;
    std::shared_ptr<grpc::ClientContext> active;
    std::atomic<bool> invalid{false}, finished{false};
    std::atomic<std::int64_t> last_frame{boot_milliseconds()};
    std::thread watcher([&] {
        while (!finished) {
            bool cancel = stop || interrupted;
            try {
                cancel = cancel || read_private_token(token_path) != token || read_file(inputs.metadata_file, 8192) != metadata_bytes ||
                         read_file(inputs.ca_file, 65536) != ca;
            } catch (...) { cancel = true; }
            if (cancel) {
                invalid = true;
                std::lock_guard<std::mutex> lock(context_mutex);
                if (active) active->TryCancel();
            }
            if (boot_milliseconds() - last_frame.load() > 250) {
                try {
                    runtime.disconnect();
                    log.state("READINESS_CHANGED", "NOT_READY", "KUKSA_DATA_UNAVAILABLE");
                } catch (...) {
                    invalid = true;
                    std::lock_guard<std::mutex> lock(context_mutex);
                    if (active) active->TryCancel();
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });
    struct Join { std::atomic<bool>& finished; std::thread& thread; ~Join() { finished = true; thread.join(); } } join{finished, watcher};
    const auto make_context = [&] {
        auto result = std::make_shared<grpc::ClientContext>();
        result->AddMetadata("authorization", "Bearer " + token);
        std::lock_guard<std::mutex> lock(context_mutex); active = result;
        if (invalid) result->TryCancel();
        return result;
    };
    val::GetRequest request; val::GetResponse response;
    for (const auto* path : paths) { auto* entry = request.add_entries(); entry->set_path(path); entry->set_view(val::VIEW_METADATA); }
    auto get_context = make_context();
    get_context->set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(8));
    const auto get_status = stub->Get(get_context.get(), request, &response);
    if (!get_status.ok()) {
        const auto code = get_status.error_code();
        throw std::runtime_error(code == grpc::StatusCode::UNAUTHENTICATED || code == grpc::StatusCode::PERMISSION_DENIED
            ? "KUKSA_AUTH_UNAVAILABLE" : "KUKSA_DATA_UNAVAILABLE");
    }
    verify_metadata(response);
    log.state("VDP_CONTRACT_ACCEPTED", "READY", "NONE");
    val::SubscribeRequest subscription;
    for (const auto* path : paths) { auto* entry = subscription.add_entries(); entry->set_path(path); entry->set_view(val::VIEW_CURRENT_VALUE); }
    auto stream_context = make_context();
    auto reader = stub->Subscribe(stream_context.get(), subscription);
    struct Cancel {
        std::shared_ptr<grpc::ClientContext> context;
        ~Cancel() { context->TryCancel(); }
    } cancel{stream_context};
    std::array<Signal, 6> values{};
    std::int64_t previous_epoch = -1;
    val::SubscribeResponse update;
    while (reader->Read(&update)) {
        if (stop || interrupted || invalid) break;
        std::set<std::size_t> seen;
        for (const auto& item : update.updates()) {
            const auto index = path_index(item.entry().path());
            if (!seen.insert(index).second) throw std::runtime_error("KUKSA_DUPLICATE_UPDATE");
            values[index] = signal(item.entry(), index);
        }
        const auto now = boot_milliseconds();
        const auto frame = complete_frame(values, wall_milliseconds(), now, previous_epoch);
        if (!frame) continue;
        const auto result = runtime.ingest(*frame);
        previous_epoch = frame->source_epoch_ms;
        if (!result.validation.valid) { runtime.disconnect(); continue; }
        last_frame = now;
        log.state("READINESS_CHANGED", "READY", "NONE");
        if (result.event_started) log.state("WINDOW_TRIGGERED", "CAPTURING", "NONE");
        if (result.completed) log.state("WINDOW_COMPLETED", brake_health::v1::terminal_state_name(result.completed->terminal_state),
                                        "NONE", result.completed->event_id);
    }
    stream_context->TryCancel();
    (void)reader->Finish();
    runtime.disconnect();
    log.state("KUKSA_SUBSCRIPTION_CHANGED", "NOT_READY", "KUKSA_DATA_UNAVAILABLE");
}
}
int main(int argc, char** argv) {
    std::atomic<bool> stop{false};
    Log log;
    try {
        const auto inputs = parse_arguments(argc, argv);
        const char* token_file = std::getenv("KUKSA_TOKEN_FILE");
        if (std::getenv("AOS_SECRET") || !token_file || std::string(token_file) != token_path) throw std::runtime_error("CREDENTIAL_BOUNDARY_INVALID");
        const auto metadata = parse_metadata(read_file(inputs.metadata_file, 8192));
        Runtime runtime("/storage/brake-health/v1/events", metadata);
        std::signal(SIGINT, signal_handler); std::signal(SIGTERM, signal_handler);
        std::thread delivery([&] { deliver(runtime, stop, log); });
        struct Join { std::atomic<bool>& stop; std::thread& thread; ~Join() { stop = true; thread.join(); } } join{stop, delivery};
        log.state("SERVICE_STARTED", "RUNNING", "NONE");
        while (!interrupted) {
            try { subscribe(runtime, inputs, stop, log); }
            catch (const std::exception& error) {
                runtime.disconnect();
                const std::string code = error.what();
                const auto reason = code == "KUKSA_AUTH_UNAVAILABLE" ? "KUKSA_AUTH_UNAVAILABLE" :
                    code == "VDP_INCOMPATIBLE" ? "VDP_INCOMPATIBLE" :
                    code == "IMMUTABLE_IDENTITY_CHANGED" || code == "INPUT_FILE_UNAVAILABLE" ? "STATE_INVALID" : "KUKSA_DATA_UNAVAILABLE";
                log.state("READINESS_CHANGED", "NOT_READY", reason);
            }
            pause(stop, 1000);
        }
        runtime.stop(); stop = true;
        log.state("SERVICE_STOPPED", "STOPPED", "NONE");
        return 0;
    } catch (...) {
        stop = true;
        log.state("READINESS_CHANGED", "NOT_READY", "STATE_INVALID");
        return 2;
    }
}
