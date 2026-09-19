// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "brake_health/runtime/application.hpp"
#include "brake_health/runtime/json.hpp"
#include "brake_health/v3/messages.hpp"

namespace brake_health::runtime {
v3::GatewayStatus parse_gateway_status(const std::string& bytes);
struct AdvisoryDelivery { std::string id; std::string bytes; };
// Caller serializes this private state with model/outbox admission. HTTP and
// KUKSA calls happen outside that lock. No alternate endpoint or motion API.
class AdvisoryRuntime {
public:
    AdvisoryRuntime(std::filesystem::path state_root, std::filesystem::path outbox_root,
                    const v2::ModelState& model);
    std::optional<v3::AdvisoryRequest> next_request(const v2::ModelState& model,
        const v1::MessageMetadata& metadata, std::int64_t now_ms);
    void written(const v3::AdvisoryRequest& request);
    bool observe(const std::string& status, std::int64_t now_ms,
                 std::size_t other_count, std::size_t other_bytes);
    std::optional<AdvisoryDelivery> next_message() const;
    bool accept(const AdvisoryDelivery& message, const HttpResponse& response);
    std::pair<std::size_t, std::size_t> outbox_usage() const;
    std::optional<std::string> current_gateway_state() const;
    std::optional<std::string> current_request_id() const;
    std::string demo_poll(const v1::MessageMetadata&) const;
    void begin_demo_reset(const std::string&,const v1::MessageMetadata&,std::int64_t now);
    void reconcile_demo_reset(const v1::MessageMetadata&);
    std::optional<std::string> reset_model_command() const;
    std::int64_t reset_deadline() const;
    void model_reset_applied();
    bool reset_pending() const;
    std::optional<std::string> reset_ack(std::int64_t now);
    void reset_acknowledged(const std::string&);
private:
    Json load() const;
    void commit(const Json& before, const Json& after, const std::optional<AdvisoryDelivery>& fact = {});
    void recover();
    std::filesystem::path state_root_, outbox_root_;
    std::string epoch_;
};
}  // namespace brake_health::runtime
