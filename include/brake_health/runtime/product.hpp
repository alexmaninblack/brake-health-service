// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "brake_health/runtime/model_capture.hpp"
#include "brake_health/runtime/advisory_runtime.hpp"
#include "brake_health/runtime/derived_delivery.hpp"
#include <memory>

namespace brake_health::runtime {
enum class FunctionalProfile { V1, V2, V3 };
FunctionalProfile functional_profile(const std::string& value);
struct ProductObservation {
    bool valid{}, event_started{}, event_completed{};
    std::optional<v2::ProcessResult> analysis;
};
struct ProductDelivery {
    enum class Kind { Window, Derived, Advisory } kind;
    PendingMessage window;
    v2::OutboxEntry derived;
    AdvisoryDelivery advisory;
    std::string bytes() const;
};
class Product {
public:
    Product(std::filesystem::path storage, v1::MessageMetadata metadata, FunctionalProfile profile,
            v1::UuidSource uuid = random_uuid);
    ProductObservation ingest(const std::vector<Signal>& values, std::int64_t wall, std::int64_t monotonic);
    void update_metadata(const v1::MessageMetadata& metadata);
    void disconnect();
    void stop();
    bool analytics_ready() const;
    std::optional<ProductDelivery> next_message();
    bool accept(const ProductDelivery& delivery, const HttpResponse& response);
    std::optional<v3::AdvisoryRequest> next_request(std::int64_t now);
    void request_written(const v3::AdvisoryRequest& request);
    bool observe_gateway(const std::string& bytes, std::int64_t now);
    std::optional<std::string> gateway_state() const;
    std::optional<v2::ModelState> model_state() const;
    FunctionalProfile profile() const { return profile_; }
private:
    std::pair<std::size_t, std::size_t> derived_usage() const;
    std::filesystem::path root_;
    v1::MessageMetadata metadata_;
    FunctionalProfile profile_;
    Runtime legacy_;
    ModelCapture capture_;
    std::unique_ptr<v2::StateStore> model_;
    std::unique_ptr<AdvisoryRuntime> advisory_;
    std::int64_t previous_epoch_{-1};
    bool ready_{};
    unsigned delivery_cursor_{};
    mutable std::mutex mutex_;
};
}  // namespace brake_health::runtime
