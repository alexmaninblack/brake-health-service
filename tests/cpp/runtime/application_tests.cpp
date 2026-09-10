// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0
#include "brake_health/runtime/application.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
void product_contract_tests();

namespace {
using namespace brake_health::runtime;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(#x) + " at " + std::to_string(__LINE__)); } while (false)
template<class F> void rejects(F call) { bool failed = false; try { call(); } catch (...) { failed = true; } CHECK(failed); }
std::string metadata() {
    return "{\"schemaVersion\":1,\"unitSystemUid\":\"fixture-unit\",\"unitRole\":\"validation\",\"serviceVersion\":\"1.0.0\",\"serviceArtifactSha256\":\"" +
        std::string(64, 'a') + "\",\"vdpContractVersion\":\"0.1.1\",\"vdpContractSha256\":\"" + std::string(64, 'b') + "\"}";
}
void closed_metadata() {
    const auto parsed = parse_metadata(metadata());
    CHECK(parsed.unit_system_uid == "fixture-unit");
    CHECK(parsed.unit_role == brake_health::v1::UnitRole::Validation);
    auto input = metadata(); input.pop_back(); input += ",\"extra\":true}";
    rejects([&] { parse_metadata(input); });
    input = metadata(); input.replace(input.find("validation"), 10, "test");
    rejects([&] { parse_metadata(input); });
    input = metadata(); input.replace(input.find(std::string(64, 'a')), 64, "wrong");
    rejects([&] { parse_metadata(input); });
    input = metadata(); input.replace(input.find("fixture-unit"), 12, "../wrong");
    rejects([&] { parse_metadata(input); });
    input = metadata(); input.replace(input.find("1.0.0"), 5, "latest");
    rejects([&] { parse_metadata(input); });
    rejects([] { parse_metadata("{}"); });
}
void arguments() {
    char name[] = "service", metadata_option[] = "--metadata-file", metadata_path[] = "/run/test/metadata.json";
    char ca_option[] = "--ca-file", ca_path[] = "/run/test/public.pem", relative[] = "relative";
    char* valid[]{name, metadata_option, metadata_path, ca_option, ca_path};
    CHECK(parse_arguments(5, valid).metadata_file == metadata_path);
    rejects([&] { parse_arguments(1, valid); });
    rejects([&] { parse_arguments(4, valid); });
    valid[4] = relative; rejects([&] { parse_arguments(5, valid); });
    valid[4] = ca_path; valid[3] = metadata_option; rejects([&] { parse_arguments(5, valid); });
}
void lease_deadlines() {
    Lease lease;
    CHECK(lease.expired(1000, 4000));
    Credential credential; credential.expires = 1300; credential.renew_after = 1180;
    lease.issued(credential, 1000, 4000);
    CHECK(lease.renew_boot == 184000);
    CHECK(!lease.expired(1299, 303999));
    CHECK(lease.expired(100, 304000));
    CHECK(lease.expired(1400, 5000));
    CHECK(lease.expired(1300, 5000));
}
void token_delivery() {
    auto pattern = (std::filesystem::temp_directory_path() / "bhs-application-XXXXXX").string();
    const auto* made = ::mkdtemp(pattern.data()); CHECK(made);
    const std::filesystem::path directory(made), token = directory / "token.jwt";
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::filesystem::remove_all(path); } } cleanup{directory};
    rejects([&] { read_private_token(token); });
    atomic_private_file(token, "e30.e30.c2ln");
    CHECK(read_private_token(token) == "e30.e30.c2ln");
    CHECK(::chmod(token.c_str(), 0600) == 0);
    rejects([&] { read_private_token(token); });
    atomic_private_file(token, "e30.e30.bmV3");
    CHECK(read_private_token(token) == "e30.e30.bmV3");
    atomic_private_file(token, "e30.e30.bmV3\n");
    rejects([&] { read_private_token(token); });
    std::filesystem::remove(token);
    std::filesystem::create_symlink(directory / "other", token);
    rejects([&] { read_private_token(token); });
}
void provenance_change() {
    auto pattern = (std::filesystem::temp_directory_path() / "bhs-provenance-XXXXXX").string();
    const auto* made = ::mkdtemp(pattern.data()); CHECK(made);
    const std::filesystem::path directory(made);
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::filesystem::remove_all(path); } } cleanup{directory};
    auto current = parse_metadata(metadata());
    Runtime runtime(directory, current);
    current.vdp_contract_sha256 = std::string(64, 'c');
    runtime.update_vdp_metadata(current);
    current.unit_system_uid = "different-unit";
    rejects([&] { runtime.update_vdp_metadata(current); });
}
}
int main() {
    try { closed_metadata(); arguments(); lease_deadlines(); token_delivery(); provenance_change(); product_contract_tests(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "13 application/product contract groups passed\n";
}
