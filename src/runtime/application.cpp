// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0
#include "brake_health/runtime/application.hpp"
#include "brake_health/runtime/json.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <ctime>
#include <fcntl.h>
#include <regex>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace brake_health::runtime {
ApplicationInputs parse_arguments(int argc, char** argv) {
    ApplicationInputs result;
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc) throw std::invalid_argument("CONFIG_ARGUMENTS_INVALID");
        const std::string option = argv[i];
        auto* destination = option == "--metadata-file" ? &result.metadata_file : option == "--ca-file" ? &result.ca_file : nullptr;
        if (!destination || !destination->empty()) throw std::invalid_argument("CONFIG_ARGUMENTS_INVALID");
        *destination = argv[i + 1];
        if (!destination->is_absolute()) throw std::invalid_argument("CONFIG_PATH_INVALID");
    }
    if (result.metadata_file.empty() || result.ca_file.empty()) throw std::invalid_argument("CONFIG_INPUTS_REQUIRED");
    return result;
}
v1::MessageMetadata parse_legacy_metadata(const std::string& bytes) {
    const auto json = parse_json(bytes, 8192);
    if (json.object().size() != 7 || json.at("schemaVersion").integer() != 1) throw std::invalid_argument("METADATA_SCHEMA_INVALID");
    v1::MessageMetadata result;
    result.unit_system_uid = json.at("unitSystemUid").string();
    const auto role = json.at("unitRole").string();
    result.service_version = json.at("serviceVersion").string();
    result.service_artifact_sha256 = json.at("serviceArtifactSha256").string();
    result.vdp_contract_version = json.at("vdpContractVersion").string();
    result.vdp_contract_sha256 = json.at("vdpContractSha256").string();
    const std::regex identifier("[A-Za-z0-9][A-Za-z0-9_.:-]{0,127}");
    const std::regex version("(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)(-[0-9A-Za-z]+([.-][0-9A-Za-z]+)*)?(\\+[0-9A-Za-z]+([.-][0-9A-Za-z]+)*)?");
    if (!std::regex_match(result.unit_system_uid, identifier) ||
        (role != "validation" && role != "production") || result.service_version.size() > 64 ||
        result.vdp_contract_version.size() > 64 || !std::regex_match(result.service_version, version) ||
        !std::regex_match(result.vdp_contract_version, version) ||
        !is_sha256(result.service_artifact_sha256) || !is_sha256(result.vdp_contract_sha256)) throw std::invalid_argument("METADATA_FIELDS_INVALID");
    result.unit_role = role == "validation" ? v1::UnitRole::Validation : v1::UnitRole::Production;
    return result;
}

ServiceInstance parse_service_instance(const Json& value) {
    if (value.object().size() != 4) throw std::invalid_argument("NATIVE_IDENTITY_INVALID");
    const auto index = value.at("instanceIndex").integer();
    if (index < 0) throw std::invalid_argument("NATIVE_IDENTITY_INVALID");
    ServiceInstance instance{value.at("serviceId").string(), value.at("subjectId").string(),
        static_cast<std::uint64_t>(index), value.at("instanceId").string()};
    if (!native_instance_valid(instance)) throw std::invalid_argument("NATIVE_IDENTITY_INVALID");
    return instance;
}
NativeServiceInputs parse_service_inputs(const std::string& bytes, const std::map<std::string, std::string>& environment) {
    const auto release = parse_json(bytes, 1024);
    if (release.object().size() != 2 || release.at("schemaVersion").integer() != 1 ||
        !package_version(release.at("serviceVersion").string()))
        throw std::invalid_argument("PACKAGE_RELEASE_INVALID");
    if (environment.size() != 4) throw std::invalid_argument("NATIVE_IDENTITY_REQUIRED");
    const auto& index = environment.at("AOS_INSTANCE_INDEX");
    if (index.empty() || index.size() > 16 || (index.size() > 1 && index.front() == '0') ||
        !std::all_of(index.begin(), index.end(), [](char c) { return c >= '0' && c <= '9'; }))
        throw std::invalid_argument("NATIVE_IDENTITY_INVALID");
    ServiceInstance instance{environment.at("AOS_ITEM_ID"), environment.at("AOS_SUBJECT_ID"),
        std::stoull(index), environment.at("AOS_INSTANCE_ID")};
    if (!native_instance_valid(instance)) throw std::invalid_argument("NATIVE_IDENTITY_INVALID");
    return {release.at("serviceVersion").string(), instance};
}
void initialize_service_inputs(ApplicationInputs& inputs) {
    std::map<std::string, std::string> environment;
    for (const auto* key : {"AOS_ITEM_ID", "AOS_SUBJECT_ID", "AOS_INSTANCE_INDEX", "AOS_INSTANCE_ID"}) {
        const char* value = std::getenv(key);
        if (!value || !*value) throw std::invalid_argument("NATIVE_IDENTITY_REQUIRED");
        environment.emplace(key, value);
    }
    inputs.native = parse_service_inputs(read_file("/usr/share/aosedge/service-release.json", 1024), environment);
}
v1::MessageMetadata parse_metadata(const std::string& bytes, const NativeServiceInputs& native) {
    const auto json = parse_json(bytes, 8192);
    if (json.object().size() != 5 || json.at("schemaVersion").integer() != 2)
        throw std::invalid_argument("METADATA_SCHEMA_INVALID");
    v1::MessageMetadata result;
    result.unit_system_uid = json.at("unitSystemUid").string();
    const auto role = json.at("unitRole").string();
    result.vdp_contract_version = json.at("vdpContractVersion").string();
    result.vdp_contract_sha256 = json.at("vdpContractSha256").string();
    if (!native_identifier(result.unit_system_uid) || (role != "validation" && role != "production") ||
        !package_version(result.vdp_contract_version) || !is_sha256(result.vdp_contract_sha256) ||
        !package_version(native.service_version) || !native_instance_valid(native.instance))
        throw std::invalid_argument("METADATA_FIELDS_INVALID");
    result.unit_role = role == "validation" ? v1::UnitRole::Validation : v1::UnitRole::Production;
    result.service_version = native.service_version;
    result.service_instance = native.instance;
    return result;
}
v1::MessageMetadata runtime_metadata(const ApplicationInputs& inputs, const std::string& bytes) {
    if (!inputs.native) throw std::invalid_argument("NATIVE_IDENTITY_REQUIRED");
    return parse_metadata(bytes, *inputs.native);
}
Json metadata_binding(const v1::MessageMetadata& m) {
    Json::Object value{{"schemaVersion", Json{std::int64_t{m.service_instance ? 2 : 1}}},
        {"unitSystemUid", Json{m.unit_system_uid}}, {"unitRole", Json{std::string(m.unit_role == v1::UnitRole::Validation ? "validation" : "production")}},
        {"serviceVersion", Json{m.service_version}}, {"vdpContractVersion", Json{m.vdp_contract_version}},
        {"vdpContractSha256", Json{m.vdp_contract_sha256}}};
    if (m.service_instance) {
        if (!native_provenance_valid(m.service_instance, m.service_version, m.service_artifact_sha256))
            throw std::invalid_argument("NATIVE_IDENTITY_INVALID");
        value.emplace("serviceInstance", parse_json(service_instance_json(*m.service_instance)));
    } else value.emplace("serviceArtifactSha256", Json{m.service_artifact_sha256});
    const Json result{value};
    (void)parse_metadata_binding(result);
    return result;
}
v1::MessageMetadata parse_metadata_binding(const Json& value) {
    // Private persisted request provenance, never a fresh public-input fallback.
    if (value.at("schemaVersion").integer() == 1) return parse_legacy_metadata(encode_json(value));
    if (value.object().size() != 7 || value.at("schemaVersion").integer() != 2)
        throw std::invalid_argument("ADVISORY_PROVENANCE_INVALID");
    const NativeServiceInputs native{value.at("serviceVersion").string(), parse_service_instance(value.at("serviceInstance"))};
    auto public_value = value.object();
    public_value.erase("serviceVersion");
    public_value.erase("serviceInstance");
    return parse_metadata(encode_json(Json{public_value}), native);
}

std::string read_private_token(const std::filesystem::path& path) {
    const int directory = open_private_token_directory(path.parent_path());
    struct CloseDirectory { int fd; ~CloseDirectory() { ::close(fd); } } close_directory{directory};
    const int fd = ::openat(directory, path.filename().c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) throw std::runtime_error("KUKSA_AUTH_UNAVAILABLE");
    struct Close { int fd; ~Close() { ::close(fd); } } close{fd};
    struct stat info{};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != ::geteuid() || info.st_nlink != 1 ||
        (info.st_mode & 07777) != 0400 || info.st_size <= 0 || info.st_size > 16384) throw std::runtime_error("KUKSA_AUTH_UNAVAILABLE");
    std::string token;
    char chunk[4096];
    for (;;) {
        const auto n = ::read(fd, chunk, sizeof(chunk));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) throw std::runtime_error("KUKSA_AUTH_UNAVAILABLE");
        if (n == 0) break;
        token.append(chunk, static_cast<std::size_t>(n));
        if (token.size() > 16384) throw std::runtime_error("KUKSA_AUTH_UNAVAILABLE");
    }
    if (token.empty() || token.front() == '.' || token.back() == '.' || token.find("..") != std::string::npos ||
        std::count(token.begin(), token.end(), '.') != 2 || !std::all_of(token.begin(), token.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        })) throw std::runtime_error("KUKSA_AUTH_UNAVAILABLE");
    return token;
}
namespace {
std::int64_t milliseconds(clockid_t clock) {
    timespec now{};
    if (::clock_gettime(clock, &now) != 0) throw std::runtime_error("CLOCK_UNAVAILABLE");
    return static_cast<std::int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}
}
std::int64_t wall_milliseconds() { return milliseconds(CLOCK_REALTIME); }
std::int64_t boot_milliseconds() {
#ifdef __linux__
    return milliseconds(CLOCK_BOOTTIME);
#else
    return milliseconds(CLOCK_MONOTONIC);  // Host compile/tests only; product is Linux.
#endif
}
void Lease::issued(const Credential& c, std::int64_t wall, std::int64_t boot) {
    expires_wall = c.expires;
    expires_boot = boot + (c.expires - wall) * 1000;
    renew_boot = boot + (c.renew_after - wall) * 1000;
}
bool Lease::expired(std::int64_t wall, std::int64_t boot) const {
    return expires_wall == 0 || wall >= expires_wall || boot >= expires_boot;
}
}  // namespace brake_health::runtime
