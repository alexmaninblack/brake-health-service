// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0
#include "brake_health/runtime/demo_mock.hpp"
#include "brake_health/v1/sha256.hpp"
#include <cassert>
#include <unistd.h>
using namespace brake_health::runtime;
int main(int argc, char** argv) {
    const bool emit = argc == 2 && std::string(argv[1]) == "--emit";
    const auto native = parse_service_inputs(R"({"schemaVersion":1,"serviceVersion":"7.0.0"})",
        {{"AOS_ITEM_ID","brake-service"},{"AOS_SUBJECT_ID","brake-subject"},{"AOS_INSTANCE_INDEX","0"},{"AOS_INSTANCE_ID","brake-instance"}});
    auto metadata = parse_metadata(std::string(R"({"schemaVersion":2,"unitSystemUid":"mock-test-unit","unitRole":"validation","vdpContractVersion":"18.0.0","vdpContractSha256":")") + std::string(64,'a') + "\"}", native);
    demo_mock::require_test(metadata);
    auto wrong = metadata; wrong.unit_role = brake_health::v1::UnitRole::Production;
    bool rejected = false; try { demo_mock::require_test(wrong); } catch (...) { rejected = true; } assert(rejected);
    wrong = metadata; wrong.service_instance.reset(); rejected = false;
    try { demo_mock::require_test(wrong); } catch (...) { rejected = true; } assert(rejected);
    char path[] = "/tmp/brake-mock-tests-XXXXXX"; assert(::mkdtemp(path));
    for (auto profile : {FunctionalProfile::V1, FunctionalProfile::V2, FunctionalProfile::V3}) {
        const auto root = std::filesystem::path(path) / std::to_string(static_cast<int>(profile));
        std::string retained;
        {
            Product product(root, metadata, profile);
            demo_mock::generate(product, 1789162000000LL, 20000);
            auto pending = product.next_message(); assert(pending);
            retained = pending->bytes();
            assert(!product.accept(*pending, {503, "", 0}));
        }
        Product product(root, metadata, profile);
        unsigned count = 0;
        while (auto pending = product.next_message()) {
            const auto bytes = pending->bytes(); const auto value = parse_json(bytes);
            assert(value.at("schemaVersion").integer() == 2);
            assert(value.at("serviceVersion").string() == "7.0.0");
            const auto kind = value.at("messageType").string();
            assert(kind != "BRAKE_ADVISORY_FACT");
            if (emit) std::cout << bytes << '\n';
            const auto identity = value.at(kind == "BRAKE_HEALTH_ASSESSMENT" ? "assessmentId" : "eventId").string();
            auto key = '[' + quote_json(metadata.unit_system_uid) + ',' + quote_json(kind) + ',' + quote_json(identity);
            if (kind == "WINDOW_CHUNK") key += ',' + std::to_string(value.at("content").at("chunkIndex").integer());
            key += ']';
            const auto ack = "{\"schemaVersion\":1,\"contractVersion\":\"1.0.0\",\"receiptId\":\"00000000-0000-4000-8000-000000000001\",\"messageKeySha256\":" + quote_json(brake_health::v1::sha256_hex(key)) + ",\"contentSha256\":" + quote_json(value.at("contentSha256").string()) + ",\"state\":\"DURABLE_ACCEPTED\",\"receivedAt\":\"2026-09-11T21:00:00Z\"}";
            assert(product.accept(*pending, {201, ack, 0})); ++count;
        }
        assert(count > 0 && !retained.empty()); assert(!product.gateway_state());
    }
    std::filesystem::remove_all(path);
}
