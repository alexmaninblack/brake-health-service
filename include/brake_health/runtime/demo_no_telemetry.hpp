// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "brake_health/runtime/application.hpp"
#include <csignal>
#include <iostream>
#include <thread>

namespace brake_health::runtime {
namespace demo_lifecycle {
inline volatile std::sig_atomic_t stopped = 0;
inline void stop(int) { stopped = 1; }
}
// Explicit Test-only lifecycle experiment, never an authentication fallback.
// No analytics child, token, network request, derived record or advisory.
inline int run_demo_no_telemetry(const v1::MessageMetadata& metadata) {
    if (metadata.unit_role != v1::UnitRole::Validation)
        throw std::runtime_error("DEMO_NO_TELEMETRY_TEST_ONLY");
    demo_lifecycle::stopped = 0;
    const auto previous_int = std::signal(SIGINT, demo_lifecycle::stop);
    const auto previous_term = std::signal(SIGTERM, demo_lifecycle::stop);
    std::cout << "{\"schemaVersion\":1,\"eventType\":\"DEMO_LIFECYCLE_ONLY\","
                 "\"currentState\":\"NOT_READY\",\"reasonCode\":\"TELEMETRY_DISABLED\","
                 "\"serviceVersion\":\"" << metadata.service_version << "\"}" << std::endl;
    while (!demo_lifecycle::stopped)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::signal(SIGINT, previous_int);
    std::signal(SIGTERM, previous_term);
    return 0;
}
}
