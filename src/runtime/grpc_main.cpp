// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0
#include "brake_health/runtime/application.hpp"
#include "brake_health/runtime/product.hpp"
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
const std::vector<std::string>& telemetry_paths() {
    static const std::vector<std::string> result = functional_profile(BHS_FUNCTIONAL_PROFILE) == FunctionalProfile::V1
        ? std::vector<std::string>(paths.begin(), paths.end()) : std::vector<std::string>(model_paths.begin(), model_paths.end());
    return result;
}
bool integer_path(std::size_t index) {
    return functional_profile(BHS_FUNCTIONAL_PROFILE) == FunctionalProfile::V1 ? index >= 4 : index == 2;
}
class Log {
    std::mutex mutex_;
    std::mutex capabilities_mutex_;
    std::map<std::string, std::string> previous_;
    std::int64_t minute_{};
    unsigned emitted_{}, suppressed_{};
    bool analytics_{}, backend_{}, advisory_{};
    std::string analytics_reason_{"STARTING"}, advisory_reason_{"VISS_OR_GATEWAY_UNAVAILABLE"};
    void readiness() {
        const bool requires_advisory = functional_profile(BHS_FUNCTIONAL_PROFILE) == FunctionalProfile::V3;
        const auto mode = !analytics_ ? "NOT_READY" : (!backend_ || (requires_advisory && !advisory_)) ? "DEGRADED" : "OPERATIONAL";
        const auto reason = !analytics_ ? analytics_reason_ : requires_advisory && !advisory_ ? advisory_reason_ : "NONE";
        state("READINESS_CHANGED", mode, reason);
    }
public:
    void input_rejected(const std::vector<Signal>& values, std::int64_t wall) {
        // Fixed quality diagnostics only: no signal values, identifiers or credentials.
        std::string reason = "COHERENCE_OR_DOMAIN_INVALID";
        if (std::any_of(values.begin(), values.end(), [](const auto& v) { return !v.valid; })) reason = "MISSING_VALUE";
        else if (std::any_of(values.begin(), values.end(), [&](const auto& v) { return v.epoch_ms > wall; })) reason = "FUTURE_TIMESTAMP";
        else if (std::any_of(values.begin(), values.end(), [&](const auto& v) { return wall - v.epoch_ms > brake_health::v1::kMaximumSourceAgeMs; })) reason = "STALE_TIMESTAMP";
        else if (std::any_of(values.begin(), values.end(), [&](const auto& v) { return v.epoch_ms != values.front().epoch_ms; })) reason = "MIXED_TIMESTAMPS";
        state("KUKSA_INPUT_REJECTED", "NOT_READY", reason);
    }
    void analytics(bool ready, const std::string& reason = "NONE") {
        std::lock_guard<std::mutex> lock(capabilities_mutex_); analytics_ = ready; analytics_reason_ = reason; readiness();
    }
    void backend(bool connected) {
        std::lock_guard<std::mutex> lock(capabilities_mutex_); backend_ = connected;
        state("BACKEND_SYNC_CHANGED", connected ? "CONNECTED" : "BACKLOG", "NONE"); readiness();
    }
    void advisory(bool ready, const std::string& reason = "NONE") {
        std::lock_guard<std::mutex> lock(capabilities_mutex_); advisory_ = ready; advisory_reason_ = reason; readiness();
    }
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
    const auto& wanted = telemetry_paths();
    const auto position = std::find(wanted.begin(), wanted.end(), path);
    if (position == wanted.end()) throw std::runtime_error("KUKSA_UNREQUESTED_PATH");
    return static_cast<std::size_t>(position - wanted.begin());
}
void verify_metadata(const val::GetResponse& response) {
    if (response.has_error() || response.errors_size() || response.entries_size() != static_cast<int>(telemetry_paths().size())) throw std::runtime_error("VDP_INCOMPATIBLE");
    std::set<std::size_t> seen;
    for (const auto& entry : response.entries()) {
        const auto index = path_index(entry.path());
        const auto& metadata = entry.metadata();
        const std::string unit = functional_profile(BHS_FUNCTIONAL_PROFILE) == FunctionalProfile::V1
            ? (index == 0 ? "km/h" : index < 4 ? "m/s^2" : "percent")
            : (index == 0 || index >= 8 ? "km/h" : index == 1 ? "m/s^2" : index == 2 ? "percent" : index == 3 ? "degrees" : "degrees/s");
        if (!seen.insert(index).second || !entry.has_metadata() || metadata.entry_type() != val::ENTRY_TYPE_SENSOR ||
            metadata.data_type() != (integer_path(index) ? val::DATA_TYPE_UINT8 : val::DATA_TYPE_FLOAT) ||
            !metadata.has_unit() || metadata.unit() != unit) throw std::runtime_error("VDP_INCOMPATIBLE");
    }
}
Signal signal(const val::DataEntry& entry, std::size_t index) {
    const auto& value = entry.value();
    if (!entry.has_value() || !value.has_timestamp() || value.timestamp().seconds() < 0 ||
        value.timestamp().seconds() > 253402300799LL || value.timestamp().nanos() < 0 || value.timestamp().nanos() >= 1000000000) return {};
    // Protobuf represents VSS uint8 as uint32; no numeric coercions are accepted.
    if ((!integer_path(index) && value.value_case() != val::Datapoint::kFloat) ||
        (integer_path(index) && value.value_case() != val::Datapoint::kUint32)) return {};
    return {!integer_path(index) ? static_cast<double>(value.float_()) : static_cast<double>(value.uint32()),
            value.timestamp().seconds() * 1000 + value.timestamp().nanos() / 1000000, true};
}
void deliver(Product& runtime, std::atomic<bool>& stop, Log& log) {
    std::mt19937 random(std::random_device{}());
    std::uniform_real_distribution<double> jitter(-0.2, 0.2);
    unsigned attempt = 0;
    while (!stop && !interrupted) {
        try {
            const auto pending = runtime.next_message();
            if (!pending) { attempt = 0; pause(stop, 100); continue; }
            HttpResponse response;
            try { response = post_backend(pending->bytes(), stop); } catch (...) {}
            if (runtime.accept(*pending, response)) {
                attempt = 0; log.backend(true);
            } else {
                log.backend(false);
                pause(stop, retry_delay(attempt++, jitter(random), response.retry_after) * 1000LL);
            }
        } catch (...) {
            log.analytics(false, "STORAGE_UNAVAILABLE");
            pause(stop, 1000);
        }
    }
}
// One context per session, watched independently of blocking gRPC calls.
// Token loss/change cancels the subscription, including a stalled Get/Read.
void subscribe(Product& runtime, const ApplicationInputs& inputs, std::atomic<bool>& stop, Log& log) {
    const auto metadata_bytes = read_file(inputs.metadata_file, 8192);
    runtime.update_metadata(runtime_metadata(inputs, metadata_bytes));
    const auto ca = read_file(inputs.ca_file, 65536);
    const auto token = read_private_token(token_file_from_environment());
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
                cancel = cancel || read_private_token(token_file_from_environment()) != token || read_file(inputs.metadata_file, 8192) != metadata_bytes ||
                         read_file(inputs.ca_file, 65536) != ca;
            } catch (...) { cancel = true; }
            if (cancel) {
                invalid = true;
                std::lock_guard<std::mutex> lock(context_mutex);
                if (active) active->TryCancel();
            }
            const auto freshness = functional_profile(BHS_FUNCTIONAL_PROFILE) == FunctionalProfile::V1
                ? brake_health::v1::kMaximumSourceAgeMs : 250;
            if (boot_milliseconds() - last_frame.load() > freshness) {
                try {
                    runtime.disconnect();
                    log.analytics(false, "KUKSA_DATA_UNAVAILABLE");
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
    for (const auto& path : telemetry_paths()) { auto* entry = request.add_entries(); entry->set_path(path); entry->set_view(val::VIEW_METADATA); }
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
    for (const auto& path : telemetry_paths()) {
        auto* entry = subscription.add_entries();
        entry->set_path(path);
        entry->set_view(val::VIEW_CURRENT_VALUE);
        // KUKSA 0.5.0 Subscribe consumes fields, not the Get view expansion.
        entry->add_fields(val::FIELD_VALUE);
    }
    auto stream_context = make_context();
    auto reader = stub->Subscribe(stream_context.get(), subscription);
    struct Cancel {
        std::shared_ptr<grpc::ClientContext> context;
        ~Cancel() { context->TryCancel(); }
    } cancel{stream_context};
    std::vector<Signal> values(telemetry_paths().size());
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
        const auto result = runtime.ingest(values, wall_milliseconds(), now);
        if (!result.valid) {
            log.input_rejected(values, wall_milliseconds());
            if (!runtime.analytics_ready()) log.analytics(false, "KUKSA_DATA_UNAVAILABLE");
            if (result.event_completed) log.state("WINDOW_COMPLETED", "INCOMPLETE_SOURCE_GAP", "NONE");
            continue;
        }
        last_frame = now;
        log.analytics(true);
        if (result.event_started) log.state("WINDOW_TRIGGERED", "CAPTURING", "NONE");
        if (result.event_completed) log.state("WINDOW_COMPLETED", "COMPLETED", "NONE");
        if (result.analysis) {
            const auto& a = *result.analysis;
            if (a.status == brake_health::v2::ProcessStatus::Produced) log.state("ASSESSMENT_CREATED", "READY", "NONE");
            if (a.event_created) log.state("CONDITION_BAND_CHANGED", "INSPECTION_RECOMMENDED", "NONE");
            if (a.status == brake_health::v2::ProcessStatus::SkippedInputQuality)
                log.state("ASSESSMENT_SKIPPED_INPUT_QUALITY", "READY",
                    a.skip_reason ? brake_health::v2::skip_reason_name(*a.skip_reason) : "INPUT_QUALITY_INSUFFICIENT");
            if (a.status == brake_health::v2::ProcessStatus::DerivedOutboxFull) log.state("DERIVED_OUTBOX_FULL", "OVERFLOW", "NONE");
        }
    }
    stream_context->TryCancel();
    (void)reader->Finish();
    runtime.disconnect();
    log.analytics(false, "KUKSA_DATA_UNAVAILABLE");
    log.state("KUKSA_SUBSCRIPTION_CHANGED", "NOT_READY", "KUKSA_DATA_UNAVAILABLE");
}
// Independent read-only GatewayStatus subscription plus own-endpoint writer.
// Telemetry capture and backend delivery never wait for this internal chain.
void advisory_session(Product& runtime, const ApplicationInputs& inputs, std::atomic<bool>& stop, Log& log) {
    const auto metadata_bytes = read_file(inputs.metadata_file, 8192);
    runtime.update_metadata(runtime_metadata(inputs, metadata_bytes));
    const auto ca = read_file(inputs.ca_file, 65536), token = read_private_token(token_file_from_environment());
    grpc::SslCredentialsOptions tls; tls.pem_root_certs = ca;
    grpc::ChannelArguments args; args.SetMaxReceiveMessageSize(8192);
    auto channel = grpc::CreateCustomChannel("Server:55555", grpc::SslCredentials(tls), args);
    auto stub = val::VAL::NewStub(channel);
    std::mutex contexts_mutex;
    std::shared_ptr<grpc::ClientContext> active, stream;
    std::string attempted_request_id;
    std::atomic<bool> finished{false}, invalid{false};
    std::thread watcher([&] {
        while (!finished) {
            bool cancel = stop || interrupted;
            try { cancel = cancel || read_private_token(token_file_from_environment()) != token || read_file(inputs.metadata_file, 8192) != metadata_bytes ||
                read_file(inputs.ca_file, 65536) != ca; } catch (...) { cancel = true; }
            if (cancel || invalid) {
                invalid = true;
                std::lock_guard<std::mutex> lock(contexts_mutex);
                if (active) active->TryCancel();
                if (stream) stream->TryCancel();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });
    struct WatcherJoin { std::atomic<bool>& done; std::thread& worker; ~WatcherJoin() { done = true; worker.join(); } } watcher_join{finished, watcher};
    const auto context = [&] {
        auto result = std::make_shared<grpc::ClientContext>();
        result->AddMetadata("authorization", "Bearer " + token);
        result->set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
        std::lock_guard<std::mutex> lock(contexts_mutex); active = result;
        if (invalid) result->TryCancel();
        return result;
    };
    // Restart ordering: observe the authoritative current status before any
    // persisted request retry or lease refresh. Empty initial leaf is absence,
    // never a fabricated NONE/APPLIED observation.
    val::GetRequest get; val::GetResponse response;
    auto* target = get.add_entries(); target->set_path(brake_health::v3::kStatusPath); target->set_view(val::VIEW_ALL);
    const auto get_context = context(); const auto status = stub->Get(get_context.get(), get, &response);
    if (!status.ok() || response.has_error() || response.errors_size() || response.entries_size() != 1)
        throw std::runtime_error("VISS_OR_GATEWAY_UNAVAILABLE");
    const auto& entry = response.entries(0);
    if (entry.path() != brake_health::v3::kStatusPath || !entry.has_metadata() ||
        entry.metadata().data_type() != val::DATA_TYPE_STRING || entry.metadata().entry_type() != val::ENTRY_TYPE_SENSOR)
        throw std::runtime_error("VDP_V3_INCOMPATIBLE");
    const auto observe = [&](const val::DataEntry& value) {
        if (value.path() != brake_health::v3::kStatusPath) throw std::runtime_error("GATEWAY_STATUS_WRONG_ENDPOINT");
        if (!value.has_value() || value.value().value_case() != val::Datapoint::kString) return;
        if (value.value().string().empty()) return;
        if (runtime.observe_gateway(value.value().string(), wall_milliseconds())) {
            // Reconcile a cached status fact without calling it fresh chain
            // readiness. Only a reply to an attempted request in this session
            // proves the capability; command outcome is still independent.
            const auto observed = parse_gateway_status(value.value().string());
            {
                std::lock_guard<std::mutex> lock(contexts_mutex);
                if (attempted_request_id == observed.request_id) log.advisory(true);
            }
            const auto evidence = runtime.gateway_state();
            if (evidence) log.state("ADVISORY_GATEWAY_STATUS", *evidence, "NONE");
        }
    };
    observe(entry);
    val::SubscribeRequest subscribe_request;
    auto* wanted = subscribe_request.add_entries(); wanted->set_path(brake_health::v3::kStatusPath); wanted->set_view(val::VIEW_CURRENT_VALUE);
    wanted->add_fields(val::FIELD_VALUE);
    auto reader_context = std::make_shared<grpc::ClientContext>(); reader_context->AddMetadata("authorization", "Bearer " + token);
    {
        std::lock_guard<std::mutex> lock(contexts_mutex); stream = reader_context;
        if (invalid) stream->TryCancel();
    }
    auto reader = stub->Subscribe(reader_context.get(), subscribe_request);
    std::thread observations([&] {
        try {
            val::SubscribeResponse update;
            while (reader->Read(&update)) {
                if (invalid || stop || interrupted) break;
                for (const auto& item : update.updates()) observe(item.entry());
            }
        } catch (...) { log.advisory(false, "VISS_OR_GATEWAY_UNAVAILABLE"); }
        invalid = true; reader_context->TryCancel(); (void)reader->Finish();
    });
    struct ReaderJoin {
        std::shared_ptr<grpc::ClientContext> context; std::thread& worker;
        ~ReaderJoin() { context->TryCancel(); worker.join(); }
    } reader_join{reader_context, observations};
    auto last_attempt = std::int64_t{-1000};
    while (!invalid && !stop && !interrupted) {
        const auto now = boot_milliseconds();
        if (now - last_attempt >= 1000) {
            const auto request = runtime.next_request(wall_milliseconds());
            if (request) {
                last_attempt = now;
                val::SetRequest set; val::SetResponse result;
                auto* update = set.add_updates(); update->add_fields(val::FIELD_ACTUATOR_TARGET);
                update->mutable_entry()->set_path(brake_health::v3::kRequestPath);
                update->mutable_entry()->mutable_actuator_target()->set_string(request->canonical_json);
                {
                    std::lock_guard<std::mutex> lock(contexts_mutex); attempted_request_id = request->request_id;
                }
                const auto set_context = context(); const auto outcome = stub->Set(set_context.get(), set, &result);
                if (outcome.ok() && !result.has_error() && !result.errors_size()) {
                    runtime.request_written(*request);
                    log.state("ADVISORY_REQUESTED", "REQUESTED", "NONE", request->decision_id);
                } else {
                    // Do not report application or select an alternate path.
                    // An uncertain write keeps the exact persisted request.
                    const bool denied = outcome.error_code() == grpc::StatusCode::PERMISSION_DENIED ||
                        outcome.error_code() == grpc::StatusCode::UNAUTHENTICATED;
                    log.advisory(false, denied ? "KUKSA_WRITE_UNAUTHORIZED" : "VISS_OR_GATEWAY_UNAVAILABLE");
                }
            }
        }
        pause(stop, 100);
    }
    log.advisory(false, "VISS_OR_GATEWAY_UNAVAILABLE");
}
void advisory(Product& runtime, const ApplicationInputs& inputs, std::atomic<bool>& stop, Log& log) {
    if (runtime.profile() != FunctionalProfile::V3) return;
    while (!stop && !interrupted) {
        try { advisory_session(runtime, inputs, stop, log); }
        catch (const std::exception& error) {
            const auto reason = std::string(error.what()) == "VDP_V3_INCOMPATIBLE" ? "VDP_V3_INCOMPATIBLE" : "VISS_OR_GATEWAY_UNAVAILABLE";
            log.advisory(false, reason);
        }
        pause(stop, 1000);
    }
}
}
int main(int argc, char** argv) {
    std::atomic<bool> stop{false};
    Log log;
    try {
        auto inputs = parse_arguments(argc, argv);
        initialize_service_inputs(inputs);
        if (std::getenv("AOS_SECRET")) throw std::runtime_error("CREDENTIAL_BOUNDARY_INVALID");
        (void)token_file_from_environment();
        const auto metadata = runtime_metadata(inputs, read_file(inputs.metadata_file, 8192));
        Product runtime("/storage/brake-health", metadata, functional_profile(BHS_FUNCTIONAL_PROFILE));
        std::signal(SIGINT, signal_handler); std::signal(SIGTERM, signal_handler);
        std::thread delivery([&] { deliver(runtime, stop, log); });
        struct Join { std::atomic<bool>& stop; std::thread& thread; ~Join() { stop = true; thread.join(); } } join{stop, delivery};
        std::thread advisory_worker([&] { advisory(runtime, inputs, stop, log); });
        Join advisory_join{stop, advisory_worker};
        log.state("SERVICE_STARTED", "RUNNING", "NONE");
        while (!interrupted) {
            try { subscribe(runtime, inputs, stop, log); }
            catch (const std::exception& error) {
                runtime.disconnect();
                const std::string code = error.what();
                const auto reason = code == "KUKSA_AUTH_UNAVAILABLE" ? "KUKSA_AUTH_UNAVAILABLE" :
                    code == "VDP_INCOMPATIBLE" ? "VDP_INCOMPATIBLE" :
                    code == "IMMUTABLE_IDENTITY_CHANGED" || code == "INPUT_FILE_UNAVAILABLE" ? "STATE_INVALID" : "KUKSA_DATA_UNAVAILABLE";
                log.analytics(false, reason);
            }
            pause(stop, 1000);
        }
        runtime.stop(); stop = true;
        log.state("SERVICE_STOPPED", "STOPPED", "NONE");
        return 0;
    } catch (...) {
        stop = true;
        log.analytics(false, "STATE_INVALID");
        return 2;
    }
}
