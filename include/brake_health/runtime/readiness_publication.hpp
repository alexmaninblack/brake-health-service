// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <optional>

namespace brake_health::runtime {
// One writer per authenticated session. A rejected/uncertain Set is never an
// acknowledgement. Evaluate live readiness each loop; keep no telemetry queue.
class ReadinessPublication {
public:
    bool due(bool ready, std::int64_t now) const {
        return now >= next_attempt_ &&
            (!accepted_ || *accepted_ != ready || now >= next_heartbeat_);
    }
    void completed(bool ready, std::int64_t now, bool accepted) {
        next_attempt_ = now + (accepted ? 100 : 1000);
        if (accepted) {
            accepted_ = ready;
            next_heartbeat_ = now + 5000;
        }
    }
private:
    std::optional<bool> accepted_;
    std::int64_t next_attempt_{};
    std::int64_t next_heartbeat_{};
};
} // namespace brake_health::runtime
