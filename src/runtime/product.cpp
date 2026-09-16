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
      legacy_(root_ / "v1/events", metadata_, uuid), capture_(uuid) {
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
        }
    }
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
    if (d.kind == ProductDelivery::Kind::Window) return legacy_.accept(d.window, response);
    if (d.kind == ProductDelivery::Kind::Derived && model_) return accept_derived_message(*model_, d.derived, response);
    return advisory_ && d.kind == ProductDelivery::Kind::Advisory && advisory_->accept(d.advisory, response);
}
std::optional<v3::AdvisoryRequest> Product::next_request(std::int64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!advisory_ || !model_->ready()) return {};
    return advisory_->next_request(model_->state(), metadata_, now);
}
void Product::request_written(const v3::AdvisoryRequest& request) {
    std::lock_guard<std::mutex> lock(mutex_); if (advisory_) advisory_->written(request);
}
bool Product::observe_gateway(const std::string& bytes, std::int64_t now) {
    std::lock_guard<std::mutex> lock(mutex_); if (!advisory_) return false;
    const auto usage = derived_usage(); return advisory_->observe(bytes, now, usage.first, usage.second);
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
}  // namespace brake_health::runtime
