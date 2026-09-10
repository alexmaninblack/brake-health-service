// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0
#include "brake_health/runtime/advisory_runtime.hpp"
#include "brake_health/v1/sha256.hpp"
#include "brake_health/v2/state_store.hpp"
#include <algorithm>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

namespace brake_health::runtime {
namespace {
void sync_dir(const std::filesystem::path& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) throw std::runtime_error("ADVISORY_STORAGE_UNAVAILABLE");
    const auto result = ::fsync(fd); const auto closed = ::close(fd);
    if (result || closed) throw std::runtime_error("ADVISORY_STORAGE_UNAVAILABLE");
}
void directory(const std::filesystem::path& path) {
    std::filesystem::create_directories(path);
    struct stat info{};
    if (::lstat(path.c_str(), &info) || !S_ISDIR(info.st_mode) || info.st_uid != ::geteuid() || ::chmod(path.c_str(), 0700))
        throw std::runtime_error("ADVISORY_STORAGE_INVALID");
}
void write(const std::filesystem::path& path, const std::string& bytes) {
    // Reuse the fsync/rename implementation; persistent facts are not tokens.
    atomic_private_file(path, bytes, 0600);
    sync_dir(path.parent_path());
}
std::string read(const std::filesystem::path& path, std::size_t limit) {
    struct stat info{};
    if (::lstat(path.c_str(), &info) || !S_ISREG(info.st_mode) || info.st_uid != ::geteuid() ||
        (info.st_mode & 0777) != 0600) throw std::runtime_error("ADVISORY_STORAGE_INVALID");
    return read_file(path, limit);
}
Json::Object& object(Json& j) { return std::get<Json::Object>(j.value); }
Json::Array& requests(Json& j) { return std::get<Json::Array>(object(j).at("requests").value); }
const Json::Array& requests(const Json& j) { return std::get<Json::Array>(j.at("requests").value); }
v3::AdvisoryRequest request(const Json& j) {
    const auto sequence = j.at("sequence").integer();
    if (j.object().size() != 12 || j.at("schemaVersion").integer() != 1 || sequence <= 0 ||
        j.at("operation").string() != "SET" || j.at("reasonCode").string() != "PREDICTED_BRAKE_DEGRADATION" ||
        j.at("recommendation").string() != "INSPECTION_RECOMMENDED" || j.at("modelVersion").string() != "brake-condition-demo-v1")
        throw std::runtime_error("ADVISORY_REQUEST_INVALID");
    auto result = v3::build_set_request(j.at("producerEpoch").string(), static_cast<std::uint64_t>(sequence),
        j.at("decisionId").string(), j.at("issuedAt").string(), j.at("serviceVersion").string());
    if (result.canonical_json != encode_json(j)) throw std::runtime_error("ADVISORY_REQUEST_INVALID");
    return result;
}
Json metadata_json(const v1::MessageMetadata& m) {
    return Json{Json::Object{{"unitSystemUid", Json{m.unit_system_uid}},
        {"unitRole", Json{std::string(m.unit_role == v1::UnitRole::Validation ? "validation" : "production")}},
        {"schemaVersion", Json{std::int64_t{1}}}, {"serviceVersion", Json{m.service_version}},
        {"serviceArtifactSha256", Json{m.service_artifact_sha256}}, {"vdpContractVersion", Json{m.vdp_contract_version}},
        {"vdpContractSha256", Json{m.vdp_contract_sha256}}}};
}
v3::DeploymentMetadata metadata(const Json& j) {
    const auto m = parse_metadata(encode_json(j));
    return {m.unit_system_uid, m.unit_role == v1::UnitRole::Validation ? v2::UnitRole::Validation : v2::UnitRole::Production,
        m.service_version, m.service_artifact_sha256, m.vdp_contract_version, m.vdp_contract_sha256};
}
bool fact_id(const std::string& name) {
    if (name.size() < 38 || !v3::canonical_uuid(name.substr(0, 36), '5') || name[36] != '.') return false;
    const auto tail = name.substr(37);
    return tail == "RECEIVED" || tail == "APPLIED" || tail == "CLEARED" || tail == "REJECTED" || tail == "EXPIRED" || tail == "FAILED";
}
}
v3::GatewayStatus parse_gateway_status(const std::string& bytes) {
    const auto j = parse_json(bytes, 1024);
    if (j.object().size() != 10 || j.at("schemaVersion").integer() != 1 || j.at("sequence").integer() <= 0)
        throw std::invalid_argument("GATEWAY_STATUS_INVALID");
    v3::GatewayStatus s;
    s.request_id = j.at("requestId").string(); s.producer_epoch = j.at("producerEpoch").string();
    s.sequence = static_cast<std::uint64_t>(j.at("sequence").integer());
    bool found = false;
    for (auto state : {v3::GatewayState::Received, v3::GatewayState::Applied, v3::GatewayState::Cleared,
                      v3::GatewayState::Rejected, v3::GatewayState::Expired, v3::GatewayState::Failed}) {
        if (j.at("state").string() == v3::gateway_state_name(state)) { s.state = state; found = true; }
    }
    if (!found) throw std::invalid_argument("GATEWAY_STATUS_INVALID");
    found = false;
    for (auto reason : {v3::GatewayReason::None, v3::GatewayReason::UnauthorizedSource, v3::GatewayReason::UnauthorizedPath,
        v3::GatewayReason::InvalidSchema, v3::GatewayReason::InvalidValue, v3::GatewayReason::StaleRequest,
        v3::GatewayReason::ReplayDetected, v3::GatewayReason::SequenceRollback, v3::GatewayReason::RateLimited,
        v3::GatewayReason::QmPolicyDenied, v3::GatewayReason::InternalError}) {
        if (j.at("reason").string() == v3::gateway_reason_name(reason)) { s.reason = reason; found = true; }
    }
    if (!found) throw std::invalid_argument("GATEWAY_STATUS_INVALID");
    s.gateway_observed_at = j.at("gatewayObservedAt").string();
    const auto recommendation = j.at("activeRecommendation").string(), reason = j.at("activeReasonCode").string();
    if (recommendation == "INSPECTION_RECOMMENDED") s.active_recommendation = v3::ActiveRecommendation::InspectionRecommended;
    else if (recommendation != "NONE") throw std::invalid_argument("GATEWAY_STATUS_WRONG_ENDPOINT");
    if (reason == "PREDICTED_BRAKE_DEGRADATION") s.active_reason = v3::ActiveReason::PredictedBrakeDegradation;
    else if (reason != "NONE") throw std::invalid_argument("GATEWAY_STATUS_WRONG_ENDPOINT");
    if (!std::holds_alternative<std::nullptr_t>(j.at("activeUntil").value)) s.active_until = j.at("activeUntil").string();
    v3::validate_gateway_status(s);
    return s;
}
AdvisoryRuntime::AdvisoryRuntime(std::filesystem::path state_root, std::filesystem::path outbox_root, const v2::ModelState& model)
    : state_root_(std::move(state_root)), outbox_root_(std::move(outbox_root)), epoch_(model.producer_epoch) {
    (void)v2::state_json(model);
    directory(state_root_); directory(outbox_root_);
    if (!std::filesystem::exists(state_root_ / "state.json")) {
        if (std::filesystem::exists(state_root_ / "journal.json") || !std::filesystem::is_empty(outbox_root_))
            throw std::runtime_error("ADVISORY_STATE_MISSING");
        write(state_root_ / "state.json", encode_json(Json{Json::Object{{"schemaVersion", Json{std::int64_t{1}}},
            {"producerEpoch", Json{epoch_}}, {"nextSequence", Json{static_cast<std::int64_t>(model.next_advisory_sequence)}},
            {"requests", Json{Json::Array{}}}}}));
    }
    recover(); (void)load(); (void)outbox_usage();
}
Json AdvisoryRuntime::load() const {
    const auto bytes = read(state_root_ / "state.json", 65536);
    auto j = parse_json(bytes, 65536);
    if (encode_json(j) != bytes || j.object().size() != 4 || j.at("schemaVersion").integer() != 1 ||
        j.at("producerEpoch").string() != epoch_ || !v3::canonical_uuid(epoch_, '4') ||
        j.at("nextSequence").integer() <= 0 || requests(j).size() > 32) throw std::runtime_error("ADVISORY_STATE_INVALID");
    std::uint64_t sequence = 0;
    for (const auto& binding : requests(j)) {
        if (binding.object().size() != 4) throw std::runtime_error("ADVISORY_STATE_INVALID");
        const auto r = request(binding.at("request"));
        if (r.producer_epoch != epoch_ || r.sequence <= sequence || r.sequence >= static_cast<std::uint64_t>(j.at("nextSequence").integer()))
            throw std::runtime_error("ADVISORY_SEQUENCE_INVALID");
        sequence = r.sequence;
        (void)metadata(binding.at("metadata")); (void)binding.at("written").boolean();
        if (binding.at("statuses").object().size() > 6) throw std::runtime_error("ADVISORY_STATE_INVALID");
        for (const auto& item : binding.at("statuses").object()) {
            const auto status = parse_gateway_status(encode_json(item.second.at("status")));
            if (item.second.object().size() != 2 || !fact_id(r.request_id + '.' + item.first) ||
                status.request_id != r.request_id || status.producer_epoch != epoch_ || status.sequence != r.sequence ||
                item.first != v3::gateway_state_name(status.state)) throw std::runtime_error("ADVISORY_STATE_INVALID");
            (void)item.second.at("admitted").boolean();
        }
    }
    return j;
}
void AdvisoryRuntime::recover() {
    const auto path = state_root_ / "journal.json";
    if (!std::filesystem::exists(path)) return;
    const auto journal = parse_json(read(path, 163840), 163840);
    if (journal.object().size() != 4 || journal.at("schemaVersion").integer() != 1) throw std::runtime_error("ADVISORY_JOURNAL_INVALID");
    const auto current = read(state_root_ / "state.json", 65536);
    const auto before = journal.at("before").string(), after = journal.at("after").string();
    if (current != before && current != after) throw std::runtime_error("ADVISORY_JOURNAL_CONFLICT");
    if (before.size() > 65536 || after.size() > 65536) throw std::runtime_error("ADVISORY_JOURNAL_INVALID");
    if (current == before) write(state_root_ / "state.json", after);
    if (!std::holds_alternative<std::nullptr_t>(journal.at("fact").value)) {
        const auto& fact = journal.at("fact"); const auto id = fact.at("id").string(), bytes = fact.at("bytes").string();
        if (fact.object().size() != 2 || !fact_id(id) || bytes.size() > 16384) throw std::runtime_error("ADVISORY_JOURNAL_INVALID");
        const auto target = outbox_root_ / id;
        if (std::filesystem::exists(target)) {
            if (read(target, 16384) != bytes) throw std::runtime_error("ADVISORY_FACT_CONFLICT");
        } else write(target, bytes);
    }
    std::filesystem::remove(path); sync_dir(state_root_);
}
void AdvisoryRuntime::commit(const Json& before, const Json& after, const std::optional<AdvisoryDelivery>& fact) {
    const auto encoded = encode_json(after);
    if (encoded.size() > 65536) throw std::runtime_error("ADVISORY_STATE_BOUND");
    if (std::filesystem::exists(state_root_ / "journal.json")) throw std::runtime_error("ADVISORY_RECOVERY_REQUIRED");
    Json payload{nullptr};
    if (fact) payload = Json{Json::Object{{"id", Json{fact->id}}, {"bytes", Json{fact->bytes}}}};
    write(state_root_ / "journal.json", encode_json(Json{Json::Object{{"schemaVersion", Json{std::int64_t{1}}},
        {"before", Json{encode_json(before)}}, {"after", Json{encoded}}, {"fact", payload}}}));
    recover();
}
std::optional<v3::AdvisoryRequest> AdvisoryRuntime::next_request(const v2::ModelState& model,
    const v1::MessageMetadata& metadata_value, std::int64_t now) {
    if (model.producer_epoch != epoch_) throw std::runtime_error("ADVISORY_EPOCH_CHANGED");
    if (model.condition_band != v2::ConditionBand::InspectionRecommended || !model.last_assessment_id) return {};
    const auto before = load(); auto after = before;
    auto& entries = requests(after);
    std::string decision = *model.last_assessment_id;
    if (!entries.empty()) {
        const auto old = request(entries.back().at("request"));
        // Refresh the accepted active decision. A newer same-band assessment
        // does not invent another band transition or activation.
        decision = old.decision_id;
        const auto elapsed = now - v3::timestamp_milliseconds(old.issued_at);
        if (elapsed < 0) return {};
        if (elapsed < static_cast<std::int64_t>(v3::kRefreshMilliseconds)) {
            if (!entries.back().at("written").boolean()) return old;
            return {};
        }
    }
    const auto sequence = before.at("nextSequence").integer();
    if (sequence == std::numeric_limits<std::int64_t>::max()) throw std::runtime_error("ADVISORY_SEQUENCE_EXHAUSTED");
    const auto r = v3::build_set_request(epoch_, static_cast<std::uint64_t>(sequence), decision, utc_timestamp(now), metadata_value.service_version);
    entries.push_back(Json{Json::Object{{"request", parse_json(r.canonical_json, 2048)},
        {"metadata", metadata_json(metadata_value)}, {"written", Json{false}}, {"statuses", Json{Json::Object{}}}}});
    while (entries.size() > 16) entries.erase(entries.begin());
    object(after)["nextSequence"] = Json{sequence + 1};
    commit(before, after); return r;
}
void AdvisoryRuntime::written(const v3::AdvisoryRequest& r) {
    const auto before = load(); auto after = before;
    for (auto& binding : requests(after)) {
        if (encode_json(binding.at("request")) == r.canonical_json) {
            if (binding.at("written").boolean()) return;
            object(binding)["written"] = Json{true}; commit(before, after); return;
        }
    }
}
bool AdvisoryRuntime::observe(const std::string& bytes, std::int64_t now, std::size_t other_count, std::size_t other_bytes) {
    const auto status = parse_gateway_status(bytes);
    if (status.producer_epoch != epoch_) return false;
    const auto before = load(); auto after = before;
    for (auto& binding : requests(after)) {
        const auto r = request(binding.at("request"));
        if (r.request_id != status.request_id || r.sequence != status.sequence) continue;
        auto& statuses = object(object(binding).at("statuses"));
        const std::string state = v3::gateway_state_name(status.state);
        const auto canonical = v3::gateway_status_json(status);
        if (statuses.count(state)) {
            if (encode_json(statuses.at(state).at("status")) != canonical) throw std::runtime_error("ADVISORY_STATUS_CONFLICT");
            return true;
        }
        const auto fact = v3::build_advisory_fact(metadata(binding.at("metadata")), r, status, utc_timestamp(now));
        const auto usage = outbox_usage();
        const bool admit = other_count <= 64 && other_bytes <= 1048576 &&
            v2::derived_outbox_admissible(usage.first + other_count, usage.second + other_bytes, 1, fact.canonical_json.size());
        statuses[state] = Json{Json::Object{{"status", parse_json(canonical, 1024)}, {"admitted", Json{admit}}}};
        object(binding)["written"] = Json{true};
        commit(before, after, admit ? std::optional<AdvisoryDelivery>{{r.request_id + '.' + state, fact.canonical_json}} : std::nullopt);
        return true;
    }
    return false;
}
std::pair<std::size_t, std::size_t> AdvisoryRuntime::outbox_usage() const {
    std::size_t count = 0, bytes = 0;
    for (const auto& entry : std::filesystem::directory_iterator(outbox_root_)) {
        const auto name = entry.path().filename().string();
        if (name.rfind('.', 0) == 0) continue;
        if (name.size() > 12 && name.substr(name.size() - 12) == ".quarantined") {
            if (!fact_id(name.substr(0, name.size() - 12))) throw std::runtime_error("ADVISORY_OUTBOX_INVALID");
            (void)read(entry.path(), 32); continue;
        }
        if (!fact_id(name)) throw std::runtime_error("ADVISORY_OUTBOX_INVALID");
        const auto encoded = read(entry.path(), 16384); const auto fact = parse_json(encoded, 16384);
        if (fact.at("requestId").string() + '.' + fact.at("gatewayState").string() != name ||
            fact.at("messageType").string() != "BRAKE_ADVISORY_FACT" ||
            v1::sha256_hex(encode_json(fact.at("content"))) != fact.at("contentSha256").string())
            throw std::runtime_error("ADVISORY_OUTBOX_INVALID");
        ++count; bytes += encoded.size();
    }
    if (count > 64 || bytes > 1048576) throw std::runtime_error("ADVISORY_OUTBOX_BOUND");
    return {count, bytes};
}
std::optional<AdvisoryDelivery> AdvisoryRuntime::next_message() const {
    (void)outbox_usage();
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(outbox_root_)) {
        if (fact_id(entry.path().filename().string()) && !std::filesystem::exists(entry.path().string() + ".quarantined")) files.push_back(entry.path());
    }
    if (files.empty()) return {};
    std::sort(files.begin(), files.end());
    return AdvisoryDelivery{files.front().filename().string(), read(files.front(), 16384)};
}
bool AdvisoryRuntime::accept(const AdvisoryDelivery& delivery, const HttpResponse& response) {
    if (!fact_id(delivery.id)) return false;
    const auto path = outbox_root_ / delivery.id;
    if (!std::filesystem::exists(path) || std::filesystem::exists(path.string() + ".quarantined") || read(path, 16384) != delivery.bytes) return false;
    if (retryable_http(response.status)) return false;
    if (!matches_ack(delivery.bytes, response)) { write(path.string() + ".quarantined", "DELIVERY_CONFLICT\n"); return false; }
    std::filesystem::remove(path); sync_dir(outbox_root_); return true;
}
std::optional<std::string> AdvisoryRuntime::current_gateway_state() const {
    const auto current = load(); if (requests(current).empty()) return {};
    const auto& statuses = requests(current).back().at("statuses").object();
    if (statuses.empty()) return {};
    const Json* latest = nullptr;
    for (const auto& value : statuses) {
        const auto& status = value.second.at("status");
        if (!latest || status.at("gatewayObservedAt").string() > latest->at("gatewayObservedAt").string()) latest = &status;
    }
    return latest->at("state").string();
}
}  // namespace brake_health::runtime
