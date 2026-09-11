// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "brake_health/runtime/application.hpp"
#include "brake_health/runtime/product.hpp"
#include <csignal>
#include <iostream>
#include <thread>

namespace brake_health::runtime {
namespace demo_mock {
inline std::atomic<bool> stopped{false};
inline void stop(int) { stopped.store(true, std::memory_order_relaxed); }
inline void require_test(const v1::MessageMetadata& metadata) {
    if (metadata.unit_role != v1::UnitRole::Validation || !metadata.service_instance)
        throw std::runtime_error("DEMO_MOCK_NATIVE_TEST_ONLY");
}
// Synthetic fixture, not an observed braking event. Timestamps end before now.
inline void generate(Product& product, std::int64_t now, std::int64_t boot) {
    product.disconnect();
    for (int i = 0; i < 280; ++i) {
        const auto time = i * 1000 / 30;
        const auto at = now - 10000 + time;
        const bool braking = time >= 3200 && time < 6200;
        const double speed = time < 3200 ? 42 : time < 6200 ? 42 - (time - 3200) * 32.0 / 3000 : 0;
        const bool v1 = product.profile() == FunctionalProfile::V1;
        std::vector<Signal> values(v1 ? 6 : 12, Signal{0, at, true});
        values[0].value = speed; values[1].value = braking ? -6 : 0;
        if (v1) values[5].value = braking ? 75 : 0;
        else {
            values[2].value = braking ? 75 : 0;
            for (std::size_t wheel = 0; wheel < 4; ++wheel) {
                values[wheel + 4].value = speed * 10 + (braking && wheel == 0 ? 30 : 0);
                values[wheel + 8].value = speed + (braking && wheel == 0 ? 3 : 0);
            }
        }
        if (!product.ingest(values, at + 20, boot - 10000 + time).valid)
            throw std::runtime_error("DEMO_MOCK_FIXTURE_INVALID");
    }
}
}
// Explicit package option, never entered on a normal authentication failure.
// The model/outbox is isolated. No KUKSA, analytics child or advisory writer.
inline int run_demo_mock(const v1::MessageMetadata& metadata) {
    demo_mock::require_test(metadata);
    Product product("/storage/brake-health/demo-mock", metadata, functional_profile(BHS_FUNCTIONAL_PROFILE));
    demo_mock::stopped = false;
    const auto old_int = std::signal(SIGINT, demo_mock::stop);
    const auto old_term = std::signal(SIGTERM, demo_mock::stop);
    std::cout << "{\"eventType\":\"DEMO_MOCK_STARTED\",\"source\":\"DEMO_MOCK\",\"vehicleTelemetry\":false,\"serviceVersion\":"
              << quote_json(metadata.service_version) << "}" << std::endl;
    std::int64_t next_episode = 0, next_attempt = 0;
    unsigned failures = 0;
    while (!demo_mock::stopped) {
        const auto boot = boot_milliseconds();
        const auto pending = product.next_message();
        if (pending && boot >= next_attempt) {
            HttpResponse response;
            try { response = post_backend(pending->bytes(), demo_mock::stopped, true); } catch (...) {}
            const bool accepted = product.accept(*pending, response);
            std::cout << "{\"eventType\":\"DEMO_MOCK_DELIVERY\",\"source\":\"DEMO_MOCK\",\"accepted\":"
                      << (accepted ? "true" : "false") << ",\"httpStatus\":" << response.status << "}" << std::endl;
            if (accepted) { failures = 0; next_attempt = 0; }
            else next_attempt = boot_milliseconds() + retry_delay(failures++, 0, response.retry_after) * 1000LL;
        } else if (!pending && boot >= next_episode) {
            demo_mock::generate(product, wall_milliseconds(), boot);
            next_episode = boot + 30000;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    product.stop();
    std::signal(SIGINT, old_int); std::signal(SIGTERM, old_term);
    return 0;
}
}
