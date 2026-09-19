// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0
#include "brake_health/runtime/product.hpp"
#include <algorithm>
#include <stdexcept>

namespace brake_health::runtime {
FunctionalProfile functional_profile(const std::string& value) {
    if (value == "v1") return FunctionalProfile::V1;
    if (value == "v2") return FunctionalProfile::V2;
    if (value == "v3") return FunctionalProfile::V3;
    throw std::invalid_argument("FUNCTIONAL_PROFILE_INVALID");
}
std::string ProductDelivery::bytes() const {
    if (kind == Kind::Window) return window.bytes;
    if (kind == Kind::Derived) return derived.canonical_json;
    return advisory.bytes;
}
Product::Product(std::filesystem::path storage, v1::MessageMetadata metadata, FunctionalProfile profile, v1::UuidSource uuid)
    : root_(std::move(storage)), metadata_(std::move(metadata)), profile_(profile),
      legacy_(root_ / "v1/events", metadata_, uuid), capture_(uuid), function_(profile==FunctionalProfile::V3) {
    if (profile_ != FunctionalProfile::V1) {
        const auto state_file = root_ / "model-state/v1/state.json";
        const auto epoch = std::filesystem::exists(state_file)
            ? v2::parse_state_json(read_file(state_file, 65536)).producer_epoch : random_uuid();
        model_ = std::make_unique<v2::StateStore>(state_file.parent_path(), root_ / "v2/outbox", epoch);
        if (!model_->ready()) throw std::runtime_error("STATE_INVALID");
        if (profile_ == FunctionalProfile::V3)
            advisory_ = std::make_unique<AdvisoryRuntime>(root_ / "advisory-state/v1", root_ / "v3/outbox", model_->state());
        complete_demo_reset(wall_milliseconds());
    }
}
std::pair<std::size_t, std::size_t> Product::derived_usage() const {
    if (!model_) return {};
    const auto inventory = model_->inventory(); std::size_t bytes = 0;
    for (const auto& entry : inventory) bytes += entry.canonical_json.size();
    return {inventory.size(), bytes};
}
ProductObservation Product::ingest(const std::vector<Signal>& values, std::int64_t wall, std::int64_t mono) {
    std::lock_guard<std::mutex> lock(mutex_);
    ProductObservation result;
    if(advisory_&&advisory_->reset_pending())return result;
    if (values.size() != (profile_ == FunctionalProfile::V1 ? 6U : 12U)) throw std::invalid_argument("SIGNAL_COUNT_INVALID");
    const auto bounds=std::minmax_element(values.begin(),values.end(),
        [](const auto& a,const auto& b){return a.epoch_ms<b.epoch_ms;});
    const auto oldest=bounds.first->epoch_ms, source=bounds.second->epoch_ms;
    const bool coherent = source-oldest <= (profile_==FunctionalProfile::V1 ? 0 : v2::kMaximumSignalSkewMs);
    const auto source_gap = [&] {
        function_.interruption("SOURCE_DISCONTINUITY");
        function_.input("CONNECTED","INVALID","INVALID_SAMPLE");
        legacy_.disconnect();
        result.event_completed = capture_.abort(v2::TerminalState::IncompleteSourceGap).has_value();
        ready_ = false;
    };
    if (std::any_of(values.begin(), values.end(), [](const auto& value) { return !value.valid; }) ||
        (coherent && (source < previous_epoch_ || source > wall || oldest<0 || wall - oldest >
            (profile_ == FunctionalProfile::V1 ? v1::kMaximumSourceAgeMs : v2::kMaximumSourceAgeMs)))) {
        source_gap(); return result;
    }
    if (profile_ == FunctionalProfile::V1) {
        if (values.size() != 6) throw std::invalid_argument("SIGNAL_COUNT_INVALID");
        std::array<Signal, 6> input; std::copy(values.begin(), values.end(), input.begin());
        const auto frame = complete_frame(input, wall, mono, previous_epoch_);
        if (!frame) { if (coherent && source > previous_epoch_) source_gap(); return result; }
        previous_epoch_ = frame->source_epoch_ms;
        const auto observation = legacy_.ingest(*frame);
        ready_ = result.valid = observation.validation.valid;
        result.event_started = observation.event_started; result.event_completed = observation.completed.has_value();
        if(observation.completed) {
            const auto& window=*observation.completed;
            const bool complete=window.terminal_state==v1::TerminalState::Complete||window.terminal_state==v1::TerminalState::TruncatedMaxDuration;
            function_.activity(complete?"COMPLETED":"SKIPPED",complete?"NONE":"SOURCE_DISCONTINUITY",window.event_id);
            const auto entries=legacy_.inventory();
            if(complete&&std::any_of(entries.begin(),entries.end(),[&](const auto& e){return e.event_id==window.event_id;}))
                function_.result("WINDOW",window.event_id,window.trigger_timestamp,metadata_.service_version);
        } else if(result.valid) {
            const auto activity=legacy_.activity();
            function_.activity(activity.first,activity.first=="WAITING"?"NOT_QUALIFIED":"NONE",activity.second);
        }
    } else {
        if (values.size() != 12) throw std::invalid_argument("SIGNAL_COUNT_INVALID");
        std::array<Signal, 12> input; std::copy(values.begin(), values.end(), input.begin());
        const auto frame = complete_model_frame(input, wall, mono, previous_epoch_);
        if (!frame || !model_->ready()) { if (coherent && source > previous_epoch_) source_gap(); return result; }
        previous_epoch_ = frame->source_epoch_ms;
        const auto was_capturing = capture_.capturing();
        const auto episode = capture_.ingest(*frame);
        ready_ = result.valid = true;telemetry_at_=wall;
        result.event_started = !was_capturing && capture_.capturing(); result.event_completed = episode.has_value();
        if (episode) {
            v2::DeploymentMetadata m{metadata_.unit_system_uid,
                metadata_.unit_role == v1::UnitRole::Validation ? v2::UnitRole::Validation : v2::UnitRole::Production,
                metadata_.service_version, metadata_.service_artifact_sha256, metadata_.vdp_contract_version,
                metadata_.vdp_contract_sha256, metadata_.service_artifact_sha256, v2::kModelConfigSha256, utc_timestamp(wall), metadata_.service_instance};
            const auto usage = advisory_ ? advisory_->outbox_usage() : std::pair<std::size_t, std::size_t>{};
            result.analysis = model_->process(*episode, m, {}, usage.first, usage.second);
            if (!model_->ready()) ready_ = result.valid = false;
            const auto& analysis=*result.analysis;
            const bool produced=analysis.status==v2::ProcessStatus::Produced;
            std::string reason="NOT_QUALIFIED";
            if(analysis.status==v2::ProcessStatus::DerivedOutboxFull||analysis.status==v2::ProcessStatus::NotReadyState)reason="STORAGE_UNAVAILABLE";
            else if(analysis.skip_reason==v2::SkipReason::InsufficientActiveSamples)reason="INSUFFICIENT_SAMPLES";
            else if(analysis.skip_reason&&analysis.skip_reason!=v2::SkipReason::InsufficientQualifiedWheelSamples)reason="INVALID_INPUT";
            function_.activity(produced?"COMPLETED":"SKIPPED",produced?"NONE":reason,episode->source_event_id);
            if(produced&&analysis.assessment_id&&!episode->samples.empty())
                function_.result("ASSESSMENT",*analysis.assessment_id,episode->samples.back().source_timestamp,metadata_.service_version);
        } else {
            const auto activity=capture_.activity_state();
            function_.activity(activity,std::string(activity)=="WAITING"?"NOT_QUALIFIED":"NONE",capture_.activity_id());
        }
    }
    if(result.valid)function_.input("CONNECTED","RECEIVING","NONE");
    return result;
}
void Product::update_metadata(const v1::MessageMetadata& m) {
    std::lock_guard<std::mutex> lock(mutex_);
    legacy_.update_vdp_metadata(m); // Enforces immutable Unit/service identity.
    if (metadata_.vdp_contract_sha256 != m.vdp_contract_sha256 || metadata_.vdp_contract_version != m.vdp_contract_version) {
        capture_.abort(v2::TerminalState::IncompleteSourceGap); previous_epoch_ = -1; ready_ = false;
    }
    metadata_ = m;
}
void Product::disconnect() {
    std::lock_guard<std::mutex> lock(mutex_); legacy_.disconnect();
    function_.interruption("SOURCE_DISCONTINUITY");
    function_.input("DISCONNECTED","DISCONNECTED","TRANSPORT_LOST");
    capture_.abort(v2::TerminalState::IncompleteSourceGap); ready_ = false;
}
void Product::stop() {
    std::lock_guard<std::mutex> lock(mutex_); legacy_.stop();
    capture_.abort(v2::TerminalState::AbortedServiceStop); ready_ = false;
}
bool Product::analytics_ready() const {
    std::lock_guard<std::mutex> lock(mutex_); return ready_ && (!model_ || model_->ready());
}
std::optional<ProductDelivery> Product::next_message() {
    std::lock_guard<std::mutex> lock(mutex_);
    // Fair selection prevents an offline legacy window from gating v2/v3 work.
    for (unsigned i = 0; i < 3; ++i) {
        const auto choice = delivery_cursor_++ % 3;
        if (choice == 0) {
            const auto message = legacy_.next_message();
            if (message) return ProductDelivery{ProductDelivery::Kind::Window, *message, {}, {}};
        } else if (choice == 1 && model_) {
            const auto message = next_derived_message(*model_);
            if (message) return ProductDelivery{ProductDelivery::Kind::Derived, {}, *message, {}};
        } else if (choice == 2 && advisory_) {
            const auto message = advisory_->next_message();
            if (message) return ProductDelivery{ProductDelivery::Kind::Advisory, {}, {}, *message};
        }
    }
    return {};
}
bool Product::accept(const ProductDelivery& d, const HttpResponse& response) {
    std::lock_guard<std::mutex> lock(mutex_);
    bool accepted=false;
    if (d.kind == ProductDelivery::Kind::Window) accepted=legacy_.accept(d.window, response);
    else if (d.kind == ProductDelivery::Kind::Derived && model_) accepted=accept_derived_message(*model_, d.derived, response);
    else if(advisory_ && d.kind == ProductDelivery::Kind::Advisory)accepted=advisory_->accept(d.advisory, response);
    function_.delivery(accepted,response.status,response.body);return accepted;
}
std::optional<v3::AdvisoryRequest> Product::next_request(std::int64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!advisory_ || !model_->ready()) return {};
    const auto request=advisory_->next_request(model_->state(), metadata_, now);
    if(request)function_.advisory("WAITING",request->request_id);
    return request;
}
void Product::request_written(const v3::AdvisoryRequest& request) {
    std::lock_guard<std::mutex> lock(mutex_); if (advisory_) advisory_->written(request);
}
bool Product::observe_gateway(const std::string& bytes, std::int64_t now) {
    std::lock_guard<std::mutex> lock(mutex_); if (!advisory_) return false;
    const auto usage = derived_usage();const bool accepted=advisory_->observe(bytes, now, usage.first, usage.second);
    if(accepted) {
        const auto status=parse_json(bytes);
        if(advisory_->current_request_id()==status.at("requestId").string()) {
            const auto state=status.at("state").string();
            function_.advisory(state=="APPLIED"||state=="CLEARED"?"CONFIRMED":state=="RECEIVED"?"WAITING":"UNAVAILABLE",status.at("requestId").string());
        }
    }
    return accepted;
}
std::optional<std::string> Product::gateway_state() const {
    std::lock_guard<std::mutex> lock(mutex_); return advisory_ ? advisory_->current_gateway_state() : std::nullopt;
}
std::optional<v2::ModelState> Product::model_state() const {
    std::lock_guard<std::mutex> lock(mutex_); return model_ ? std::optional<v2::ModelState>{model_->state()} : std::nullopt;
}
void Product::complete_demo_reset(std::int64_t now) {
    if(!advisory_)return;
    advisory_->reconcile_demo_reset(metadata_);
    if(const auto id=advisory_->reset_model_command()) {
        if(now>=advisory_->reset_deadline()&&!model_->demo_reset_applied(*id)) {
            (void)advisory_->reset_ack(now);return;
        }
        model_->reset_demo(*id);advisory_->model_reset_applied();
        capture_.reset_demo();previous_epoch_=-1;ready_=false;
        function_.activity("WAITING","RESET");
    }
}
std::optional<std::string> Product::demo_control_poll() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return advisory_&&metadata_.service_instance?std::optional<std::string>{advisory_->demo_poll(metadata_)}:std::nullopt;
}
void Product::demo_control_command(const std::string& bytes,std::int64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);if(!advisory_)throw std::runtime_error("RESET_PROFILE_UNSUPPORTED");
    advisory_->begin_demo_reset(bytes,metadata_,now);complete_demo_reset(now);
}
std::optional<std::string> Product::demo_control_ack(std::int64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);return advisory_?advisory_->reset_ack(now):std::nullopt;
}
void Product::demo_control_accepted(const std::string& bytes) {
    std::lock_guard<std::mutex> lock(mutex_);if(advisory_)advisory_->reset_acknowledged(bytes);
}
std::string Product::advisory_readiness(std::int64_t now) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return encode_json(Json{Json::Object{{"schemaVersion",Json{std::int64_t{1}}},{"ready",Json{advisory_&&ready_&&model_->ready()&&telemetry_at_>=0&&now>=telemetry_at_&&now-telemetry_at_<=5000}},
        {"observedAt",Json{utc_timestamp(now)}}}});
}
std::optional<Json> Product::observation_binding() const {
    std::lock_guard<std::mutex> lock(mutex_);if(!metadata_.service_instance)return std::nullopt;
    const std::string profile=profile_==FunctionalProfile::V1?"v1":profile_==FunctionalProfile::V2?"v2":"v3";
    return Json{Json::Object{{"messageType",Json{std::string("BRAKE_FUNCTION_OBSERVATION")}},
        {"unitSystemUid",Json{metadata_.unit_system_uid}},{"unitRole",Json{std::string(metadata_.unit_role==v1::UnitRole::Validation?"VALIDATION":"PRODUCTION")}},
        {"serviceVersion",Json{metadata_.service_version}},{"serviceProfile",Json{profile}},
        {"serviceInstance",parse_json(service_instance_json(*metadata_.service_instance))}}};
}
Json Product::function_observation() {
    std::lock_guard<std::mutex> lock(mutex_);auto usage=legacy_.delivery_usage();
    if(model_)for(const auto& entry:model_->inventory()){++usage.first;usage.second|=entry.quarantined;}
    if(advisory_)usage.first+=advisory_->outbox_usage().first;
    return function_.snapshot(usage.first,usage.second|| (model_&&!model_->ready()));
}
void Product::input_observation(const std::string& connection,const std::string& state,const std::string& reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(state!="RECEIVING")function_.interruption(reason=="REAUTHENTICATING"?"REAUTHENTICATING":"SOURCE_DISCONTINUITY");
    function_.input(connection,state,reason);
}
void Product::advisory_observation(const std::string& state) {
    std::lock_guard<std::mutex> lock(mutex_);function_.advisory(state);
}
}  // namespace brake_health::runtime
