// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "brake_health/runtime/runtime.hpp"
#include "brake_health/runtime/token_session.hpp"

namespace brake_health::runtime {
struct ApplicationInputs { std::filesystem::path metadata_file; std::filesystem::path ca_file; };
ApplicationInputs parse_arguments(int argc, char** argv);
v1::MessageMetadata parse_metadata(const std::string& bytes);
std::string read_private_token(const std::filesystem::path& path);
std::int64_t boot_milliseconds();
std::int64_t wall_milliseconds();
// A successful KAC response is translated to BOOTTIME deadlines. A wall-clock
// jump forward can revoke earlier; a backward jump must not extend the lease.
struct Lease {
    std::int64_t expires_wall{};
    std::int64_t expires_boot{};
    std::int64_t renew_boot{};
    void issued(const Credential& credential, std::int64_t wall_seconds, std::int64_t boot_ms);
    bool expired(std::int64_t wall_seconds, std::int64_t boot_ms) const;
};
}  // namespace brake_health::runtime
