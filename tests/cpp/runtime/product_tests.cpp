// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0
#include "brake_health/runtime/product.hpp"
#include "brake_health/v1/sha256.hpp"
#include <cmath>
#include <iostream>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <unistd.h>

namespace {
using namespace brake_health::runtime;
namespace v1 = brake_health::v1;
namespace v2 = brake_health::v2;
namespace v3 = brake_health::v3;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(#x) + " at product test " + std::to_string(__LINE__)); } while (false)
template<class F> void rejects(F call) { bool failed = false; try { call(); } catch (...) { failed = true; } CHECK(failed); }
struct Directory {
    std::filesystem::path path;
    Directory() {
        auto pattern = (std::filesystem::temp_directory_path() / "bhs-product-XXXXXX").string();
        const auto* made = ::mkdtemp(pattern.data()); CHECK(made); path = made;
    }
    ~Directory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};
constexpr std::int64_t epoch = 1787400000000LL;
bool native_fixture=false;
v1::MessageMetadata metadata(const std::string& release) {
    if(native_fixture) {
        const auto native=parse_service_inputs("{\"schemaVersion\":1,\"serviceVersion\":"+quote_json(release)+"}",
            {{"AOS_ITEM_ID","brake-service"},{"AOS_SUBJECT_ID","group-subject"},{"AOS_INSTANCE_INDEX","0"},{"AOS_INSTANCE_ID","native-"+release}});
        return parse_metadata("{\"schemaVersion\":2,\"unitSystemUid\":\"host-test-unit\",\"unitRole\":\"validation\",\"vdpContractVersion\":\"42.0.0\",\"vdpContractSha256\":\""+std::string(64,'2')+"\"}",native);
    }
    return {"host-test-unit", v1::UnitRole::Validation, release, std::string(64, '1'), "42.0.0", std::string(64, '2')};
}
std::vector<Signal> sample(std::int64_t time) {
    const bool braking = time >= 3200 && time < 6200;
    const double speed = time < 3200 ? 42 : time < 6200 ? 42 - (time - 3200) * 32.0 / 3000 : 0;
    const auto at = epoch + time;
    std::vector<Signal> result(12, Signal{0, at, true});
    result[0].value = speed; result[1].value = braking ? -6 : 0;
    result[2].value = braking ? 75 : 0;
    for (std::size_t i = 0; i < 4; ++i) {
        result[i + 4].value = speed * 10 + (braking && i == 0 ? 30 : 0);
        result[i + 8].value = speed + (braking && i == 0 ? 3 : 0);
    }
    return result;
}
v2::ProcessResult drive(Product& product) {
    std::optional<v2::ProcessResult> result;
    for (int i = 0; i < 280; ++i) {
        const auto time = i * 1000 / 30;
        const auto observation = product.ingest(sample(time), epoch + time + 20, time);
        CHECK(observation.valid);
        if (observation.analysis) result = observation.analysis;
    }
    CHECK(result); return *result;
}
std::string ack(const std::string& bytes) {
    const auto message = parse_json(bytes); const auto kind = message.at("messageType").string();
    const auto id = message.at(kind == "BRAKE_HEALTH_ASSESSMENT" ? "assessmentId" : kind == "BRAKE_ADVISORY_FACT" ? "requestId" : "eventId").string();
    auto key = '[' + quote_json(message.at("unitSystemUid").string()) + ',' + quote_json(kind) + ',' + quote_json(id);
    if (kind == "BRAKE_ADVISORY_FACT") key += ',' + quote_json(message.at("gatewayState").string());
    key += ']';
    return "{\"schemaVersion\":1,\"contractVersion\":\"1.0.0\",\"receiptId\":\"00000000-0000-4000-8000-000000000002\","
        "\"messageKeySha256\":" + quote_json(v1::sha256_hex(key)) + ",\"contentSha256\":" + quote_json(message.at("contentSha256").string()) +
        ",\"state\":\"DURABLE_ACCEPTED\",\"receivedAt\":\"2026-09-10T00:00:00Z\"}";
}
v3::GatewayStatus applied(const v3::AdvisoryRequest& r, std::int64_t at) {
    return {r.request_id, r.producer_epoch, r.sequence, v3::GatewayState::Applied, v3::GatewayReason::None, utc_timestamp(at),
        v3::ActiveRecommendation::InspectionRecommended, v3::ActiveReason::PredictedBrakeDegradation, r.expires_at};
}
void adapter_contract() {
    CHECK(functional_profile("v1") == FunctionalProfile::V1);
    CHECK(functional_profile("v2") == FunctionalProfile::V2);
    CHECK(functional_profile("v3") == FunctionalProfile::V3);
    rejects([] { functional_profile("3.0.0"); }); rejects([] { functional_profile("latest"); });
    CHECK(quantize_milli(1.2345, -10, 10) == 1235);
    CHECK(quantize_milli(-1.2345, -10, 10) == -1235);
    CHECK(quantize_milli(-0.0, -10, 10) == 0);
    CHECK(quantize_milli(75.0005, 0, 100) == 75001);
    rejects([] { quantize_milli(std::numeric_limits<double>::quiet_NaN(), -10, 10); });
    rejects([] { quantize_milli(std::numeric_limits<double>::infinity(), -10, 10); });
    rejects([] { quantize_milli(101, 0, 100); });
    const auto input = sample(4000); std::array<Signal, 12> values;
    std::copy(input.begin(), input.end(), values.begin());
    CHECK(complete_model_frame(values, epoch + 4250, 4000, epoch + 3999));
    CHECK(complete_model_frame(values, epoch + 9000, 4000, epoch + 3999));
    CHECK(!complete_model_frame(values, epoch + 9001, 4000, epoch + 3999));
    values[11].valid = false; CHECK(!complete_model_frame(values, epoch + 4020, 4000, epoch + 3999));
    values[11].valid = true; --values[11].epoch_ms;
    const auto mixed=complete_model_frame(values, epoch+4020,4000,epoch+3999);
    CHECK(mixed && mixed->source_age_ms==21 && mixed->source_epoch_ms==epoch+4000);
    values[11].epoch_ms=epoch+3900;CHECK(complete_model_frame(values,epoch+4020,4000,epoch+3999));
    values[11].epoch_ms=epoch+3899;CHECK(!complete_model_frame(values,epoch+4020,4000,epoch+3999));
    values[11].epoch_ms=epoch+4021;CHECK(!complete_model_frame(values,epoch+4020,4000,epoch+3999));
    rejects([] { encode_json(Json{0.25}); });
}
void product_upgrade_and_delivery() {
    Directory directory;
    v2::ModelState initial;
    {
        Product product(directory.path, metadata("21.0.0"), FunctionalProfile::V2);
        const auto result = drive(product);
        CHECK(result.status == v2::ProcessStatus::Produced && result.event_created);
        CHECK(product.analytics_ready());
        const auto function=product.function_observation();
        CHECK(function.at("input").at("state").string()=="RECEIVING");
        CHECK(function.at("activity").at("state").string()=="WAITING");
        CHECK(function.at("lastResult").at("id").string()==*result.assessment_id);
        CHECK(function.at("lastResult").at("serviceVersion").string()=="21.0.0");
        CHECK(function.at("advisory").at("state").string()=="NOT_SUPPORTED");
        initial = *product.model_state(); CHECK(initial.generation == 1 && initial.condition_band == v2::ConditionBand::InspectionRecommended);
        CHECK(!product.next_request(epoch + 10000));
        auto pending = product.next_message(); CHECK(pending && pending->kind == ProductDelivery::Kind::Derived);
        CHECK(parse_json(pending->bytes()).at("serviceVersion").string() == "21.0.0");
        CHECK(!product.accept(*pending, {503, "", 0}));
        const auto delayed=product.function_observation();
        CHECK(delayed.at("input").at("state").string()=="RECEIVING");
        CHECK(delayed.at("delivery").at("state").string()=="RETRYING");
        CHECK(product.model_state()->generation == 1);
    }
    v3::AdvisoryRequest request;
    {
        Product product(directory.path, metadata("22.0.0"), FunctionalProfile::V3);
        CHECK(v2::state_json(*product.model_state()) == v2::state_json(initial));
        const auto pending = product.next_request(epoch + 10000); CHECK(pending); request = *pending;
        CHECK(request.sequence == 1 && request.producer_epoch == initial.producer_epoch);
        CHECK(request.decision_id == *initial.last_assessment_id && request.service_version == "22.0.0");
        CHECK(product.next_request(epoch + 10500)->canonical_json == request.canonical_json);
        CHECK(!product.gateway_state());
    }
    {
        Product product(directory.path, metadata("23.0.0"), FunctionalProfile::V3);
        CHECK(product.next_request(epoch + 11000)->canonical_json == request.canonical_json);
        const auto status = applied(request, epoch + 11000);
        auto foreign = status; foreign.producer_epoch = random_uuid();
        CHECK(!product.observe_gateway(v3::gateway_status_json(foreign), epoch + 11010));
        CHECK(product.observe_gateway(v3::gateway_status_json(status), epoch + 11020));
        CHECK(product.observe_gateway(v3::gateway_status_json(status), epoch + 11030));
        CHECK(product.gateway_state() == "APPLIED");
        CHECK(product.function_observation().at("advisory").at("state").string()=="CONFIRMED");
        CHECK(!product.next_request(epoch + 29999));
        unsigned derived_count = 0, facts = 0;
        while (const auto message = product.next_message()) {
            if (message->kind == ProductDelivery::Kind::Derived) ++derived_count;
            else if (message->kind == ProductDelivery::Kind::Advisory) {
                ++facts; CHECK(parse_json(message->bytes()).at("serviceVersion").string() == "22.0.0");
            } else CHECK(false);
            CHECK(product.accept(*message, {201, ack(message->bytes()), 0}));
        }
        CHECK(derived_count == 2 && facts == 1);
        const auto refreshed = product.next_request(epoch + 30000); CHECK(refreshed);
        CHECK(refreshed->sequence == 2 && refreshed->request_id != request.request_id && refreshed->service_version == "23.0.0");
        CHECK(refreshed->decision_id == request.decision_id);
        CHECK(!product.gateway_state());
        CHECK(product.observe_gateway(v3::gateway_status_json(status), epoch + 30010));
        CHECK(!product.gateway_state()); // Old same-epoch evidence cannot replace current request status.
        CHECK(product.function_observation().at("advisory").at("state").string()=="WAITING");
        product.request_written(*refreshed); CHECK(!product.next_request(epoch + 31000));
        product.stop(); CHECK(v2::state_json(*product.model_state()) == v2::state_json(initial));
    }
    Product repeated(directory.path, metadata("24.0.0"), FunctionalProfile::V3);
    CHECK(!repeated.next_request(epoch + 32000));
    CHECK(repeated.next_request(epoch + 50000)->sequence == 3);
}
void advisory_overflow_and_conflict() {
    Directory directory;
    auto model = v2::initial_state(random_uuid()); model.wear_index = 62; model.condition_score = 38;
    model.condition_band = v2::ConditionBand::InspectionRecommended;
    model.last_assessment_id = "55e7c2c5-af19-5a79-91d5-8a09c7a6e5d4";
    model.last_applied_source_event_id = "00000000-0000-4000-8000-000000000001";
    model.recent_source_event_ids.push_back(*model.last_applied_source_event_id);
    model.generation = 1;
    AdvisoryRuntime runtime(directory.path / "state", directory.path / "outbox", model);
    const auto first = runtime.next_request(model, metadata("25.0.0"), epoch); CHECK(first);
    const auto status = applied(*first, epoch + 20);
    CHECK(runtime.observe(v3::gateway_status_json(status), epoch + 30, 64, 1));
    CHECK(!runtime.next_message() && runtime.current_gateway_state() == "APPLIED");
    // Overflow is permanent non-enqueue, not a postponed fabricated fact.
    CHECK(runtime.observe(v3::gateway_status_json(status), epoch + 40, 0, 0));
    CHECK(!runtime.next_message());
    const auto next = runtime.next_request(model, metadata("25.0.0"), epoch + 20000); CHECK(next);
    CHECK(runtime.observe(v3::gateway_status_json(applied(*next, epoch + 20020)), epoch + 20030, 0, 0));
    const auto message = runtime.next_message(); CHECK(message);
    CHECK(!runtime.accept(*message, {409, "", 0})); CHECK(!runtime.next_message());
    CHECK(runtime.outbox_usage().first == 1);
    AdvisoryRuntime reopened(directory.path / "state", directory.path / "outbox", model);
    CHECK(!reopened.next_message() && reopened.outbox_usage().first == 1);
    CHECK(reopened.next_request(model, metadata("26.0.0"), epoch + 40000)->sequence == 3);
    auto wrong = v3::gateway_status_json(status);
    wrong.pop_back(); wrong += ",\"unexpected\":true}";
    rejects([&] { parse_gateway_status(wrong); });
    wrong = v3::gateway_status_json(status);
    wrong.replace(wrong.find("INSPECTION_RECOMMENDED"), 22, "TIRE_INSPECTION_RECOMMENDED");
    rejects([&] { parse_gateway_status(wrong); });
}
void profile_one_no_model() {
    Directory directory;
    Product product(directory.path, metadata("40.0.0"), FunctionalProfile::V1);
    CHECK(!product.model_state()); CHECK(!product.next_request(epoch));
    CHECK(!std::filesystem::exists(directory.path / "model-state"));
    CHECK(!std::filesystem::exists(directory.path / "advisory-state"));
    std::vector<Signal> input{{19, epoch, true}, {0, epoch, true}, {0, epoch, true},
        {0, epoch, true}, {0, epoch, true}, {0, epoch, true}};
    CHECK(product.ingest(input, epoch + 5000, 0).valid);
    CHECK(product.function_observation().at("advisory").at("state").string()=="NOT_SUPPORTED");
    CHECK(product.function_observation().at("input").at("state").string()=="RECEIVING");
    for (auto& value : input) value.epoch_ms += 50;
    CHECK(!product.ingest(input, epoch + 5051, 50).valid);
}
v2::ModelState active_model() {
    auto model = v2::initial_state(random_uuid());
    model.wear_index = 62; model.condition_score = 38; model.condition_band = v2::ConditionBand::InspectionRecommended;
    model.last_assessment_id = "55e7c2c5-af19-5a79-91d5-8a09c7a6e5d4";
    model.last_applied_source_event_id = "00000000-0000-4000-8000-000000000001";
    model.recent_source_event_ids.push_back(*model.last_applied_source_event_id); model.generation = 1;
    return model;
}
Json journal(const std::string& before, const std::string& after, const AdvisoryDelivery& delivery) {
    const Json fact{Json::Object{{"id", Json{delivery.id}}, {"bytes", Json{delivery.bytes}}}};
    return Json{Json::Object{{"schemaVersion", Json{std::int64_t{1}}}, {"before", Json{before}}, {"after", Json{after}},
        {"fact", fact}, {"beforeSha256", Json{v1::sha256_hex(before)}}, {"afterSha256", Json{v1::sha256_hex(after)}},
        {"factSha256", Json{v1::sha256_hex(encode_json(fact))}}}};
}
void advisory_journal_recovery() {
    // Three actual persistent crash frontiers: only journal, after state, and
    // after fact. The test fixture restores those on-disk frontiers; runtime
    // recovery itself is the same code used by the product executable.
    for (int stage = 0; stage < 3; ++stage) {
        Directory directory; const auto state = directory.path / "state", outbox = directory.path / "outbox";
        const auto model = active_model();
        AdvisoryRuntime original(state, outbox, model);
        const auto request = original.next_request(model, metadata("27.0.0"), epoch); CHECK(request);
        const auto before = read_file(state / "state.json", 65536);
        const auto status = v3::gateway_status_json(applied(*request, epoch + 10));
        CHECK(original.observe(status, epoch + 20, 0, 0));
        const auto delivery = original.next_message(); CHECK(delivery);
        const auto after = read_file(state / "state.json", 65536);
        if (stage < 2) std::filesystem::remove(outbox / delivery->id);
        if (stage == 0) atomic_private_file(state / "state.json", before, 0600);
        atomic_private_file(state / "journal.json", encode_json(journal(before, after, *delivery)), 0600);
        AdvisoryRuntime recovered(state, outbox, model);
        CHECK(!std::filesystem::exists(state / "journal.json"));
        CHECK(read_file(state / "state.json", 65536) == after);
        CHECK(recovered.next_message()->bytes == delivery->bytes);
        CHECK(recovered.observe(status, epoch + 30, 0, 0)); CHECK(recovered.outbox_usage().first == 1);
        CHECK(recovered.accept(*delivery, {201, ack(delivery->bytes), 0}));
        AdvisoryRuntime repeated(state, outbox, model);
        CHECK(!repeated.next_message()); CHECK(repeated.observe(status, epoch + 40, 0, 0)); CHECK(!repeated.next_message());
        CHECK(repeated.next_request(model, metadata("28.0.0"), epoch + 20000)->sequence == 2);
    }
}
void advisory_corrupt_journal_preserved() {
    Directory directory; const auto state = directory.path / "state", outbox = directory.path / "outbox";
    const auto model = active_model();
    AdvisoryRuntime original(state, outbox, model);
    const auto request = original.next_request(model, metadata("27.0.0"), epoch); CHECK(request);
    const auto before = read_file(state / "state.json", 65536);
    CHECK(original.observe(v3::gateway_status_json(applied(*request, epoch + 10)), epoch + 20, 0, 0));
    const auto delivery = original.next_message(); CHECK(delivery);
    const auto after = read_file(state / "state.json", 65536);
    std::filesystem::remove(outbox / delivery->id);
    atomic_private_file(state / "state.json", before, 0600);
    auto value = journal(before, after, *delivery);
    std::get<Json::Object>(value.value)["afterSha256"] = Json{std::string(64, '0')};
    atomic_private_file(state / "journal.json", encode_json(value), 0600);
    rejects([&] { AdvisoryRuntime invalid(state, outbox, model); });
    CHECK(read_file(state / "state.json", 65536) == before); CHECK(std::filesystem::is_empty(outbox));
    CHECK(std::filesystem::exists(state / "journal.json"));
    // Even a self-consistent hash cannot authorize an unknown state schema.
    auto changed = parse_json(after); std::get<Json::Object>(changed.value)["schemaVersion"] = Json{std::int64_t{2}};
    value = journal(before, encode_json(changed), *delivery);
    atomic_private_file(state / "journal.json", encode_json(value), 0600);
    rejects([&] { AdvisoryRuntime invalid(state, outbox, model); });
    CHECK(read_file(state / "state.json", 65536) == before); CHECK(std::filesystem::is_empty(outbox));
}
void invalid_product_frame_ends_capture() {
    for (const bool missing : {true, false}) {
        Directory directory;
        Product product(directory.path, metadata("32.0.0"), FunctionalProfile::V2);
        for (int i = 0; i < 150; ++i) {
            const auto time = i * 1000 / 30;
            CHECK(product.ingest(sample(time), epoch + time + 20, time).valid);
        }
        auto bad = sample(5000);
        if (missing) bad[11].valid = false; else bad[11].value = std::numeric_limits<double>::quiet_NaN();
        const auto observation = product.ingest(bad, epoch + 5020, 5000);
        CHECK(!observation.valid && observation.event_completed);
        CHECK(!product.analytics_ready()); CHECK(product.model_state()->generation == 0);
        CHECK(product.function_observation().at("activity").at("state").string()=="SKIPPED");
        CHECK(product.function_observation().at("input").at("state").string()=="INVALID");
        CHECK(!product.next_message());
        product.stop(); CHECK(product.model_state()->generation == 0);
    }
}
void model_capture_retrigger_and_limit() {
    for (const bool truncate : {false, true}) {
        Directory directory;
        Product product(directory.path, metadata("34.0.0"), FunctionalProfile::V2);
        unsigned completed = 0; std::optional<v2::ProcessResult> result;
        const int frames = truncate ? 540 : 360;
        for (int i = 0; i < frames; ++i) {
            const auto time = i * 1000 / 30;
            auto input = sample(time);
            const bool braking = time >= 3200 && (truncate || time < 4800 || (time >= 5800 && time < 7200));
            input[0].value = 42; input[1].value = braking ? -8 : 0; input[2].value = braking ? 90 : 0;
            for (std::size_t wheel = 0; wheel < 4; ++wheel) { input[wheel + 4].value = 420; input[wheel + 8].value = 42; }
            const auto observed = product.ingest(input, epoch + time + 20, time); CHECK(observed.valid);
            if (observed.analysis) { ++completed; result = observed.analysis; }
        }
        CHECK(result && completed == 1);
        if (truncate) {
            CHECK(result->status == v2::ProcessStatus::SkippedInputQuality);
            CHECK(result->skip_reason == v2::SkipReason::EpisodeNotComplete);
            CHECK(product.model_state()->generation == 0 && !product.next_message());
        } else {
            CHECK(result->status == v2::ProcessStatus::Produced);
            CHECK(product.model_state()->generation == 1);
        }
    }
}
void model_capture_retains_ten_hz_at_supported_source_rates() {
    std::vector<std::size_t> active_counts;
    for (const int hz : {20, 30}) {
        ModelCapture capture;
        std::optional<v2::CompletedEpisode> completed;
        for (int i = 0; i < hz * 10; ++i) {
            const auto time = i * 1000 / hz;
            auto input = sample(time);
            std::array<Signal, 12> values;
            std::copy(input.begin(), input.end(), values.begin());
            const auto frame = complete_model_frame(values, epoch + time + 20, time, epoch + time - 1);
            CHECK(frame);
            if (auto result = capture.ingest(*frame)) completed = result;
        }
        CHECK(completed && completed->terminal_state == v2::TerminalState::Complete);
        active_counts.push_back(static_cast<std::size_t>(std::count_if(
            completed->samples.begin(), completed->samples.end(),
            [](const auto& value) { return value.phase == v2::Phase::Active; })));
    }
    CHECK(active_counts[0] == 33);
    CHECK(active_counts[1] == 33);
}
}
void product_contract_tests() {
    adapter_contract(); product_upgrade_and_delivery(); advisory_overflow_and_conflict(); profile_one_no_model();
    advisory_journal_recovery(); advisory_corrupt_journal_preserved();
    invalid_product_frame_ends_capture();
    model_capture_retrigger_and_limit();
    model_capture_retains_ten_hz_at_supported_source_rates();
}

void native_products_conformance(bool emit) {
    native_fixture=true;
    Directory directory;
    const auto current=metadata("18.0.0");
    const auto check=[&](const std::string& bytes) {
        const auto value=parse_json(bytes);
        CHECK(value.at("schemaVersion").integer()==2 && value.at("contractVersion").string()=="2.0.0");
        CHECK(!value.object().count("serviceArtifactSha256") && !value.object().count("modelArtifactSha256"));
        CHECK(parse_service_instance(value.at("serviceInstance"))==*current.service_instance);
        CHECK(value.at("serviceVersion").string()=="18.0.0");
        if(emit)std::cout<<bytes<<'\n';
    };
    v1::WindowEngine engine(random_uuid);
    for(int i=0;i<=20;++i) {
        const auto at=i*50;
        v1::SourceFrame frame{42,-6,0,0,0,60,utc_timestamp(epoch+at),epoch+at,at,20,v1::FrameQuality::ValidCompleteFrame};
        engine.ingest(frame);
    }
    const auto window=engine.abort_service_stop();CHECK(window);
    const auto messages=v1::build_growing_messages(current,*window);
    for(const auto& chunk:messages.chunks)check(chunk.canonical_json);
    check(messages.completion.canonical_json);
    // A completed v2 window must survive a new package/instance without relabelling.
    {
        v1::EventSpool spool(directory.path/"window");
        CHECK(spool.store_completed(window->event_id,messages)==v1::AdmissionResult::Stored);
    }
    Runtime recovered(directory.path/"window",metadata("19.0.0"));
    CHECK(recovered.inventory().size()==1);
    CHECK(recovered.next_message()->bytes==messages.chunks.front().canonical_json);
    auto changed=metadata("19.0.0");changed.service_instance->instance_id="different";
    rejects([&]{recovered.update_vdp_metadata(changed);});
    Product product(directory.path/"product",current,FunctionalProfile::V3);
    CHECK(drive(product).status==v2::ProcessStatus::Produced);
    const auto request=product.next_request(epoch+10000);CHECK(request);
    CHECK(parse_json(request->canonical_json).at("schemaVersion").integer()==1);
    const auto status=v3::gateway_status_json(applied(*request,epoch+10010));
    CHECK(parse_json(status).at("schemaVersion").integer()==1);
    CHECK(product.observe_gateway(status,epoch+10020));
    unsigned count=0;
    while(const auto pending=product.next_message()) {
        check(pending->bytes());CHECK(product.accept(*pending,{201,ack(pending->bytes()),0}));++count;
    }
    CHECK(count==3);
    native_fixture=false;
}
void mixed_provenance_recovery() {
    Directory directory;std::string retained;
    native_fixture=false;
    {
        Product old_product(directory.path,metadata("3.0.0"),FunctionalProfile::V3);
        CHECK(drive(old_product).status==v2::ProcessStatus::Produced);
        retained=old_product.next_message()->bytes();
        CHECK(old_product.next_request(epoch+10000));
    }
    native_fixture=true;
    {
        Product product(directory.path,metadata("18.0.0"),FunctionalProfile::V3);
        CHECK(product.next_message()->bytes()==retained);
        const auto request=product.next_request(epoch+11000);CHECK(request);
        CHECK(request->service_version=="3.0.0");
        CHECK(product.observe_gateway(v3::gateway_status_json(applied(*request,epoch+11010)),epoch+11020));
        unsigned count=0;
        while(const auto pending=product.next_message()) {
            CHECK(parse_json(pending->bytes()).at("schemaVersion").integer()==1);
            CHECK(product.accept(*pending,{201,ack(pending->bytes()),0}));++count;
        }
        CHECK(count==3);
        const auto next=product.next_request(epoch+30000);CHECK(next);
        CHECK(next->sequence==request->sequence+1 && next->producer_epoch==request->producer_epoch);
        CHECK(next->service_version=="18.0.0");
        CHECK(product.observe_gateway(v3::gateway_status_json(applied(*next,epoch+30010)),epoch+30020));
        CHECK(parse_json(product.next_message()->bytes()).at("schemaVersion").integer()==2);
    }
    native_fixture=false;
}
void reset_product_contract() {
    native_fixture=true;Directory directory;std::string command_bytes,ack_bytes;std::uint64_t generation{};
    std::string producer;v3::AdvisoryRequest clear;
    const auto now=epoch+10000;
    {
        Product product(directory.path,metadata("49.0.0"),FunctionalProfile::V3);
        CHECK(drive(product).status==v2::ProcessStatus::Produced);
        const auto warning=product.next_request(now);CHECK(warning);
        producer=warning->producer_epoch;
        auto command=parse_json(*product.demo_control_poll()).object();
        command["commandId"]=Json{random_uuid()};command["operation"]=Json{std::string("RESET_DEMO_SCENARIO")};
        command["issuedAt"]=Json{utc_timestamp(now)};command["expiresAt"]=Json{utc_timestamp(now+60000)};
        const auto envelope=[&](const Json::Object& value){return encode_json(Json{Json::Object{{"schemaVersion",Json{std::int64_t{1}}},{"command",Json{value}}}});};
        command_bytes=envelope(command);
        auto wrong=command;wrong["unitSystemUid"]=Json{std::string("production")};
        rejects([&]{product.demo_control_command(envelope(wrong),now);});
        product.demo_control_command(command_bytes,now);
        generation=product.model_state()->generation;
        CHECK(product.model_state()->wear_index==54);
        clear=*product.next_request(now);CHECK(clear.clear&&clear.sequence>warning->sequence&&clear.producer_epoch==producer);
        product.demo_control_command(command_bytes,now+10);
        CHECK(product.model_state()->generation==generation);
        CHECK(!product.demo_control_ack(now+100));
        CHECK(!product.ingest(sample(0),epoch,0).valid);
    }
    {
        Product product(directory.path,metadata("49.0.0"),FunctionalProfile::V3);
        CHECK(product.model_state()->generation==generation&&product.model_state()->producer_epoch==producer);
        const auto retried=product.next_request(now+1500);CHECK(retried&&retried->request_id==clear.request_id);
        auto status=applied(clear,now+1600);status.state=v3::GatewayState::Cleared;
        status.active_recommendation=v3::ActiveRecommendation::None;status.active_reason=v3::ActiveReason::None;status.active_until.reset();
        auto wrong=status;wrong.request_id="11111111-1111-5111-8111-111111111111";
        CHECK(!product.observe_gateway(v3::gateway_status_json(wrong),now+1600));
        CHECK(!product.demo_control_ack(now+1600));
        CHECK(product.observe_gateway(v3::gateway_status_json(status),now+1600));
        ack_bytes=*product.demo_control_ack(now+1700);
        CHECK(parse_json(ack_bytes).at("result").string()=="CLEARED");
        CHECK(!product.next_request(now+2000));
        product.demo_control_command(command_bytes,now+1800);CHECK(product.model_state()->generation==generation);
        CHECK(drive(product).status==v2::ProcessStatus::Produced);
        const auto renewed=product.next_request(now+3000);CHECK(renewed&&!renewed->clear&&renewed->sequence>clear.sequence);
        CHECK(renewed->decision_id!=clear.decision_id);
        CHECK(parse_json(product.advisory_readiness(epoch+9400)).at("ready").boolean());
        CHECK(!parse_json(product.advisory_readiness(epoch+16000)).at("ready").boolean());
        product.disconnect();CHECK(!parse_json(product.advisory_readiness(epoch+9400)).at("ready").boolean());
    }
    {
        Product product(directory.path,metadata("49.0.0"),FunctionalProfile::V3);
        CHECK(product.demo_control_ack(now+4000)==ack_bytes);
        const auto command=parse_json(command_bytes).at("command");
        product.demo_control_accepted(encode_json(Json{Json::Object{{"schemaVersion",Json{std::int64_t{1}}},{"commandId",command.at("commandId")},{"state",Json{std::string("CLEARED")}}}}));
        CHECK(!product.demo_control_ack(now+4000));
        auto next=command.object();next["commandId"]=Json{random_uuid()};
        next["issuedAt"]=Json{utc_timestamp(now+5000)};next["expiresAt"]=Json{utc_timestamp(now+65000)};
        product.demo_control_command(encode_json(Json{Json::Object{{"schemaVersion",Json{std::int64_t{1}}},{"command",Json{next}}}}),now+5000);
        CHECK(!product.next_request(now+65000));
        CHECK(parse_json(*product.demo_control_ack(now+65000)).at("result").string()=="FAILED");
    }
    native_fixture=false;
}
void source_recovery_preserves_model_and_delivery() {
    // Synthetic host proof of the service-domain boundary, not a live KUKSA
    // subscription or a claim about the active VDP functional profile.
    native_fixture=true;
    for (const auto profile : {FunctionalProfile::V2, FunctionalProfile::V3}) {
        Directory directory;
        const auto current=metadata("59.0.0");
        Product product(directory.path,current,profile);
        CHECK(drive(product).status==v2::ProcessStatus::Produced);
        const auto model=v2::state_json(*product.model_state());
        const auto pending=product.next_message();CHECK(pending);
        const auto bytes=pending->bytes();

        product.disconnect();
        CHECK(!product.analytics_ready());
        product.update_metadata(current); // Unchanged family metadata is not fresh input.
        CHECK(!product.analytics_ready());
        auto missing=sample(10000);missing[3].valid=false;
        CHECK(!product.ingest(missing,epoch+10020,10000).valid);
        CHECK(!product.analytics_ready());
        CHECK(v2::state_json(*product.model_state())==model);
        CHECK(product.next_message()->bytes()==bytes);

        const auto resumed=product.ingest(sample(10100),epoch+10120,10100);
        CHECK(resumed.valid&&!resumed.analysis&&!resumed.event_started);
        CHECK(product.analytics_ready());
        CHECK(v2::state_json(*product.model_state())==model);
        CHECK(product.next_message()->bytes()==bytes);
        if (profile==FunctionalProfile::V3)
            CHECK(parse_json(product.advisory_readiness(epoch+10120)).at("ready").boolean());
    }
    native_fixture=false;
}
void native_product_contract_tests() {
    source_recovery_preserves_model_and_delivery();
    reset_product_contract();
    native_fixture=true;
    product_upgrade_and_delivery();advisory_overflow_and_conflict();
    advisory_journal_recovery();advisory_corrupt_journal_preserved();
    native_fixture=false;
    native_products_conformance(false);mixed_provenance_recovery();
}
