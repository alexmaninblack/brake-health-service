// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "brake_health/runtime/application.hpp"
#include <atomic>
#include <exception>
#include <string_view>

namespace brake_health::runtime {
struct SubscriptionFailureObservation {
    const char* connection;
    const char* input;
    const char* reason;
};
inline SubscriptionFailureObservation subscription_failure_observation(std::string_view code) {
    if (code == "KUKSA_AUTH_PENDING") return {"STARTING", "WAITING", "AWAITING_INPUT"};
    if (code == "KUKSA_AUTH_UNAVAILABLE") return {"ACCESS_DENIED", "ACCESS_DENIED", "ACCESS_DENIED"};
    if (code == "VDP_INCOMPATIBLE") return {"DISCONNECTED", "INVALID", "INVALID_SAMPLE"};
    return {"DISCONNECTED", "DISCONNECTED", "TRANSPORT_LOST"};
}
// This classifies local input replacement only. The next RPC must still
// authenticate the new token; this is never proof of granted access.
enum class SessionInputChange { None, TokenReplaced, Unavailable };
SessionInputChange inspect_session_inputs(const ApplicationInputs& inputs,
    const std::filesystem::path& token_file, const std::string& token,
    const std::string& metadata, const std::string& ca) noexcept;

class SessionInterruption final {
    std::atomic<SessionInputChange> cause_{SessionInputChange::None};
public:
    void observe(SessionInputChange change) {
        auto previous = cause_.load();
        // Unavailable is sticky: restoration after a missing/unsafe file may
        // not reclassify a failed session as an uninterrupted planned renewal.
        while (change > previous && !cause_.compare_exchange_weak(previous, change)) {}
    }
    bool cancelled() const { return cause_.load() != SessionInputChange::None; }
    bool token_replaced() const { return cause_.load() == SessionInputChange::TokenReplaced; }
};
class ReauthenticationRequired final : public std::exception {
public:
    const char* what() const noexcept override { return "KUKSA_REAUTHENTICATING"; }
};
}  // namespace brake_health::runtime
