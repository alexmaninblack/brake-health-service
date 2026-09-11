// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <cstdint>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <tuple>

namespace brake_health {
// Correlation reported by the native Aos runtime, never an authentication claim.
struct ServiceInstance {
    std::string service_id, subject_id;
    std::uint64_t instance_index{};
    std::string instance_id;
    bool operator==(const ServiceInstance& other) const {
        return std::tie(service_id, subject_id, instance_index, instance_id) ==
               std::tie(other.service_id, other.subject_id, other.instance_index, other.instance_id);
    }
    bool operator!=(const ServiceInstance& other) const { return !(*this == other); }
};
inline bool native_identifier(const std::string& value) {
    const auto alnum = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    };
    return !value.empty() && value.size() <= 128 && alnum(value.front()) &&
        std::all_of(value.begin(), value.end(), [&](char c) {
            return alnum(c) || c == '_' || c == '.' || c == ':' || c == '-';
        });
}
inline bool package_version(const std::string& value) {
    static const std::regex pattern("(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)");
    return value.size() <= 32 && std::regex_match(value, pattern);
}
inline bool native_instance_valid(const ServiceInstance& value) {
    return native_identifier(value.service_id) && native_identifier(value.subject_id) &&
        native_identifier(value.instance_id) && value.instance_index <= 9007199254740991ULL;
}
inline std::string service_instance_json(const ServiceInstance& value) {
    if (!native_instance_valid(value)) throw std::invalid_argument("NATIVE_IDENTITY_INVALID");
    // The bounded ASCII grammar excludes quotes, backslashes and control bytes.
    return "{\"instanceId\":\"" + value.instance_id + "\",\"instanceIndex\":" +
        std::to_string(value.instance_index) + ",\"serviceId\":\"" + value.service_id +
        "\",\"subjectId\":\"" + value.subject_id + "\"}";
}
inline bool native_provenance_valid(const std::optional<ServiceInstance>& instance,
                                    const std::string& version, const std::string& legacy_digest) {
    return instance && native_instance_valid(*instance) && package_version(version) && legacy_digest.empty();
}
} // namespace brake_health
