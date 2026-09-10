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
    if (profile_ == FunctionalProfile::V1) {
        if (values.size() != 6) throw std::invalid_argument("SIGNAL_COUNT_INVALID");
        std::array<Signal, 6> input; std::copy(values.begin(), values.end(), input.begin());
        const auto frame = complete_frame(input, wall, mono, previous_epoch_);
        if (!frame) return result;
        previous_epoch_ = frame->source_epoch_ms;
        const auto observation = legacy_.ingest(*frame);
        ready_ = result.valid = observation.validation.valid;
        result.event_started = observation.event_started; result.event_completed = observation.completed.has_value();
    } else {
        if (values.size() != 12) throw std::invalid_argument("SIGNAL_COUNT_INVALID");
        std::array<Signal, 12> input; std::copy(values.begin(), values.end(), input.begin());
        const auto frame = complete_model_frame(input, wall, mono, previous_epoch_);
        if (!frame || !model_->ready()) return result;
        previous_epoch_ = frame->source_epoch_ms;
        const auto was_capturing = capture_.capturing();
        const auto episode = capture_.ingest(*frame);
        ready_ = result.valid = true;
        result.event_started = !was_capturing && capture_.capturing(); result.event_completed = episode.has_value();
        if (episode) {
            v2::DeploymentMetadata m{metadata_.unit_system_uid,
                metadata_.unit_role == v1::UnitRole::Validation ? v2::UnitRole::Validation : v2::UnitRole::Production,
                metadata_.service_version, metadata_.service_artifact_sha256, metadata_.vdp_contract_version,
                metadata_.vdp_contract_sha256, metadata_.service_artifact_sha256, v2::kModelConfigSha256, utc_timestamp(wall)};
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
}  // namespace brake_health::runtime
