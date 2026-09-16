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
    const bool clear=j.at("operation").string()=="CLEAR";
    if (j.object().size() != (clear?11U:12U) || j.at("schemaVersion").integer() != 1 || sequence <= 0 ||
        (clear ? j.at("reasonCode").string()!="CONDITION_CLEARED"||j.object().count("recommendation") :
        j.at("operation").string() != "SET" || j.at("reasonCode").string() != "PREDICTED_BRAKE_DEGRADATION" ||
        j.at("recommendation").string() != "INSPECTION_RECOMMENDED") || j.at("modelVersion").string() != "brake-condition-demo-v1")
        throw std::runtime_error("ADVISORY_REQUEST_INVALID");
    auto result = (clear?v3::build_clear_request:v3::build_set_request)(j.at("producerEpoch").string(), static_cast<std::uint64_t>(sequence),
        j.at("decisionId").string(), j.at("issuedAt").string(), j.at("serviceVersion").string());
    if (result.canonical_json != encode_json(j)) throw std::runtime_error("ADVISORY_REQUEST_INVALID");
    return result;
}
v3::DeploymentMetadata metadata(const Json& j) {
    const auto m = parse_metadata_binding(j);
    return {m.unit_system_uid, m.unit_role == v1::UnitRole::Validation ? v2::UnitRole::Validation : v2::UnitRole::Production,
        m.service_version, m.service_artifact_sha256, m.vdp_contract_version, m.vdp_contract_sha256, m.service_instance};
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
namespace {
Json validate_state(const std::string& bytes, const std::string& epoch) {
    auto j = parse_json(bytes, 65536);
    if (encode_json(j) != bytes || j.object().size() != 4+j.object().count("demoReset") || j.at("schemaVersion").integer() != 1 ||
        j.at("producerEpoch").string() != epoch || !v3::canonical_uuid(epoch, '4') ||
        j.at("nextSequence").integer() <= 0 || requests(j).size() > 32) throw std::runtime_error("ADVISORY_STATE_INVALID");
    if(j.object().count("demoReset")) {
        const auto& reset=j.at("demoReset");const auto& command=reset.at("command");
        if(reset.object().size()!=4||command.object().size()!=9||command.at("schemaVersion").integer()!=1||
           command.at("operation").string()!="RESET_DEMO_SCENARIO"||!v3::canonical_uuid(command.at("commandId").string())||
           command.at("producerEpoch").string()!=epoch||!native_identifier(command.at("unitSystemUid").string())||
           !package_version(command.at("serviceVersion").string()))throw std::runtime_error("RESET_STATE_INVALID");
        (void)parse_service_instance(command.at("serviceInstance"));
        if(v3::timestamp_milliseconds(command.at("expiresAt").string())<=v3::timestamp_milliseconds(command.at("issuedAt").string()))throw std::runtime_error("RESET_STATE_INVALID");
        (void)reset.at("modelApplied").boolean();(void)reset.at("delivered").boolean();
        if(std::holds_alternative<std::nullptr_t>(reset.at("ack").value)) {
            if(reset.at("delivered").boolean())throw std::runtime_error("RESET_STATE_INVALID");
        } else {
            const auto& ack=reset.at("ack");
            if(ack.object().size()!=9||ack.at("commandId").string()!=command.at("commandId").string()||
               (ack.at("result").string()!="CLEARED"&&ack.at("result").string()!="FAILED"))throw std::runtime_error("RESET_STATE_INVALID");
            for(const auto* key:{"schemaVersion","unitSystemUid","serviceVersion","serviceInstance","producerEpoch"})
                if(encode_json(ack.at(key))!=encode_json(command.at(key)))throw std::runtime_error("RESET_STATE_INVALID");
        }
    }
    std::uint64_t sequence = 0;
    for (const auto& binding : requests(j)) {
        if (binding.object().size() != 4) throw std::runtime_error("ADVISORY_STATE_INVALID");
        const auto r = request(binding.at("request"));
        if (r.producer_epoch != epoch || r.sequence <= sequence || r.sequence >= static_cast<std::uint64_t>(j.at("nextSequence").integer()))
            throw std::runtime_error("ADVISORY_SEQUENCE_INVALID");
        sequence = r.sequence;
        if (metadata(binding.at("metadata")).service_version != r.service_version) throw std::runtime_error("ADVISORY_PROVENANCE_INVALID");
        (void)binding.at("written").boolean();
        if (binding.at("statuses").object().size() > 6) throw std::runtime_error("ADVISORY_STATE_INVALID");
        for (const auto& item : binding.at("statuses").object()) {
            const auto status = parse_gateway_status(encode_json(item.second.at("status")));
            if (item.second.object().size() != 2 || !fact_id(r.request_id + '.' + item.first) ||
                status.request_id != r.request_id || status.producer_epoch != epoch || status.sequence != r.sequence ||
                item.first != v3::gateway_state_name(status.state)) throw std::runtime_error("ADVISORY_STATE_INVALID");
            (void)item.second.at("admitted").boolean();
        }
    }
    return j;
}
void verify_fact(const std::string& bytes, const std::string& id) {
    const auto j = parse_json(bytes, 16384);
    const auto revision = j.at("schemaVersion").integer();
    if (j.object().size() != 16 || (revision != 1 && revision != 2) ||
        j.at("messageType").string() != "BRAKE_ADVISORY_FACT" || j.at("contractVersion").string() != (revision == 2 ? "2.0.0" : "1.0.0") ||
        j.at("requestId").string() + '.' + j.at("gatewayState").string() != id) throw std::runtime_error("ADVISORY_FACT_INVALID");
    const auto& c = j.at("content");
    if (c.object().size() != 11 || c.at("operation").string() != "SET" ||
        c.at("reasonCode").string() != "PREDICTED_BRAKE_DEGRADATION" || c.at("recommendation").string() != "INSPECTION_RECOMMENDED" ||
        j.at("sequence").integer() <= 0) throw std::runtime_error("ADVISORY_FACT_INVALID");
    v3::DeploymentMetadata m;
    m.unit_system_uid = j.at("unitSystemUid").string();
    const auto role = j.at("unitRole").string();
    if (role != "VALIDATION" && role != "PRODUCTION") throw std::runtime_error("ADVISORY_FACT_INVALID");
    m.unit_role = role == "VALIDATION" ? v2::UnitRole::Validation : v2::UnitRole::Production;
    m.service_version = j.at("serviceVersion").string();
    if (revision == 2) {
        if (j.object().count("serviceArtifactSha256")) throw std::runtime_error("ADVISORY_FACT_INVALID");
        m.service_instance = parse_service_instance(j.at("serviceInstance"));
    } else {
        if (j.object().count("serviceInstance")) throw std::runtime_error("ADVISORY_FACT_INVALID");
        m.service_artifact_sha256 = j.at("serviceArtifactSha256").string();
    }
    m.vdp_contract_version = j.at("vdpContractVersion").string(); m.vdp_contract_sha256 = j.at("vdpContractSha256").string();
    const auto r = v3::build_set_request(j.at("producerEpoch").string(), static_cast<std::uint64_t>(j.at("sequence").integer()),
        c.at("decisionId").string(), c.at("issuedAt").string(), m.service_version);
    const auto status = parse_gateway_status(encode_json(Json{Json::Object{{"schemaVersion", Json{std::int64_t{1}}},
        {"requestId", j.at("requestId")}, {"producerEpoch", j.at("producerEpoch")}, {"sequence", j.at("sequence")},
        {"state", j.at("gatewayState")}, {"reason", c.at("gatewayReason")}, {"gatewayObservedAt", c.at("gatewayObservedAt")},
        {"activeRecommendation", c.at("activeRecommendation")}, {"activeReasonCode", c.at("activeReasonCode")}, {"activeUntil", c.at("activeUntil")}}}));
    if (v3::build_advisory_fact(m, r, status, j.at("recordedAt").string()).canonical_json != bytes)
        throw std::runtime_error("ADVISORY_FACT_INVALID");
}
}
Json AdvisoryRuntime::load() const { return validate_state(read(state_root_ / "state.json", 65536), epoch_); }
void AdvisoryRuntime::recover() {
    const auto path = state_root_ / "journal.json";
    if (!std::filesystem::exists(path)) return;
    const auto journal = parse_json(read(path, 327680), 327680);
    if (journal.object().size() != 7 || journal.at("schemaVersion").integer() != 1) throw std::runtime_error("ADVISORY_JOURNAL_INVALID");
    const auto current = read(state_root_ / "state.json", 65536);
    const auto before = journal.at("before").string(), after = journal.at("after").string();
    if (current != before && current != after) throw std::runtime_error("ADVISORY_JOURNAL_CONFLICT");
    if (before.size() > 65536 || after.size() > 65536 ||
        v1::sha256_hex(before) != journal.at("beforeSha256").string() ||
        v1::sha256_hex(after) != journal.at("afterSha256").string() ||
        v1::sha256_hex(encode_json(journal.at("fact"))) != journal.at("factSha256").string())
        throw std::runtime_error("ADVISORY_JOURNAL_INVALID");
    (void)validate_state(before, epoch_);
    const auto next = validate_state(after, epoch_);
    std::optional<AdvisoryDelivery> publish;
    if (!std::holds_alternative<std::nullptr_t>(journal.at("fact").value)) {
        const auto& fact = journal.at("fact"); const auto id = fact.at("id").string(), bytes = fact.at("bytes").string();
        if (fact.object().size() != 2 || !fact_id(id) || bytes.size() > 16384) throw std::runtime_error("ADVISORY_JOURNAL_INVALID");
        verify_fact(bytes, id);
        const auto parsed = parse_json(bytes, 16384);
        bool bound = false;
        for (const auto& item : requests(next)) {
            const auto r = request(item.at("request"));
            const auto state = parsed.at("gatewayState").string();
            if (r.request_id != parsed.at("requestId").string() || !item.at("statuses").object().count(state)) continue;
            const auto& binding = item.at("statuses").at(state);
            const auto status = parse_gateway_status(encode_json(binding.at("status")));
            bound = binding.at("admitted").boolean() &&
                v3::build_advisory_fact(metadata(item.at("metadata")), r, status, parsed.at("recordedAt").string()).canonical_json == bytes;
        }
        if (!bound) throw std::runtime_error("ADVISORY_JOURNAL_FACT_UNBOUND");
        publish = AdvisoryDelivery{id, bytes};
    }
    if (current == before) write(state_root_ / "state.json", after);
    if (publish) {
        const auto& id = publish->id; const auto& bytes = publish->bytes;
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
        {"before", Json{encode_json(before)}}, {"after", Json{encoded}}, {"fact", payload},
        {"beforeSha256", Json{v1::sha256_hex(encode_json(before))}}, {"afterSha256", Json{v1::sha256_hex(encoded)}},
        {"factSha256", Json{v1::sha256_hex(encode_json(payload))}}}}));
    recover();
}
std::optional<v3::AdvisoryRequest> AdvisoryRuntime::next_request(const v2::ModelState& model,
    const v1::MessageMetadata& metadata_value, std::int64_t now) {
    if (model.producer_epoch != epoch_) throw std::runtime_error("ADVISORY_EPOCH_CHANGED");
    const auto before = load(); auto after = before;
    auto& entries = requests(after);
    const bool clear=before.object().count("demoReset")&&std::holds_alternative<std::nullptr_t>(before.at("demoReset").at("ack").value);
    if(clear&&(!before.at("demoReset").at("modelApplied").boolean()||
       now>=v3::timestamp_milliseconds(before.at("demoReset").at("command").at("expiresAt").string())))return {};
    if(!clear&&(model.condition_band != v2::ConditionBand::InspectionRecommended || !model.last_assessment_id))return {};
    std::string decision = clear?before.at("demoReset").at("command").at("commandId").string():*model.last_assessment_id;
    if (!entries.empty()) {
        const auto old = request(entries.back().at("request"));
        // Refresh the accepted active decision. A newer same-band assessment
        // does not invent another band transition or activation.
        if(!clear&&!old.clear)decision = old.decision_id;
        const auto elapsed = now - v3::timestamp_milliseconds(old.issued_at);
        if (elapsed < 0) return {};
        if (old.clear==clear&&old.decision_id==decision&&elapsed < (clear?5000:static_cast<std::int64_t>(v3::kRefreshMilliseconds))) {
            if (!entries.back().at("written").boolean()) return old;
            return {};
        }
    }
    const auto sequence = before.at("nextSequence").integer();
    if (sequence == std::numeric_limits<std::int64_t>::max()) throw std::runtime_error("ADVISORY_SEQUENCE_EXHAUSTED");
    const auto r = (clear?v3::build_clear_request:v3::build_set_request)(epoch_, static_cast<std::uint64_t>(sequence), decision, utc_timestamp(now), metadata_value.service_version);
    entries.push_back(Json{Json::Object{{"request", parse_json(r.canonical_json, 2048)},
        {"metadata", metadata_binding(metadata_value)}, {"written", Json{false}}, {"statuses", Json{Json::Object{}}}}});
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
        if(r.clear) {
            statuses[state]=Json{Json::Object{{"status",parse_json(canonical,1024)},{"admitted",Json{false}}}};
            object(binding)["written"]=Json{true};
            if(after.object().count("demoReset")) {
                auto& reset=object(object(after).at("demoReset"));const auto& command=reset.at("command");
                if(r.decision_id==command.at("commandId").string()&&std::holds_alternative<std::nullptr_t>(reset.at("ack").value)&&
                   status.state==v3::GatewayState::Cleared&&status.reason==v3::GatewayReason::None&&
                   status.active_recommendation==v3::ActiveRecommendation::None&&status.active_reason==v3::ActiveReason::None&&!status.active_until) {
                    auto ack=command.object();ack.erase("operation");ack.erase("issuedAt");ack.erase("expiresAt");
                    ack["result"]=Json{std::string("CLEARED")};ack["clearRequest"]=parse_json(r.canonical_json,2048);ack["gatewayStatus"]=parse_json(canonical,1024);
                    reset["ack"]=Json{ack};
                }
            }
            commit(before,after);return true;
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
        const auto encoded = read(entry.path(), 16384); verify_fact(encoded, name);
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
std::string AdvisoryRuntime::demo_poll(const v1::MessageMetadata& m) const {
    if(!m.service_instance)throw std::runtime_error("RESET_NATIVE_IDENTITY_REQUIRED");
    return encode_json(Json{Json::Object{{"schemaVersion",Json{std::int64_t{1}}},{"unitSystemUid",Json{m.unit_system_uid}},
        {"serviceVersion",Json{m.service_version}},{"serviceInstance",parse_json(service_instance_json(*m.service_instance))},
        {"producerEpoch",Json{epoch_}}}});
}
void AdvisoryRuntime::begin_demo_reset(const std::string& bytes,const v1::MessageMetadata& m,std::int64_t now) {
    const auto response=parse_json(bytes,4096);
    if(response.object().size()!=2||response.at("schemaVersion").integer()!=1)throw std::runtime_error("RESET_INVALID_RESPONSE");
    const auto& command=response.at("command");if(std::holds_alternative<std::nullptr_t>(command.value))return;
    if(command.object().size()!=9||command.at("operation").string()!="RESET_DEMO_SCENARIO"||
       !v3::canonical_uuid(command.at("commandId").string()))throw std::runtime_error("RESET_INVALID_COMMAND");
    const auto binding=parse_json(demo_poll(m));
    for(const auto& entry:binding.object())if(encode_json(command.at(entry.first))!=encode_json(entry.second))
        throw std::runtime_error("RESET_BINDING_MISMATCH");
    const auto before=load();auto after=before;
    if(before.object().count("demoReset")&&before.at("demoReset").at("command").at("commandId").string()==command.at("commandId").string()) {
        if(encode_json(before.at("demoReset").at("command"))!=encode_json(command))throw std::runtime_error("RESET_COMMAND_CONFLICT");
        return;
    }
    const auto issued=v3::timestamp_milliseconds(command.at("issuedAt").string()),expires=v3::timestamp_milliseconds(command.at("expiresAt").string());
    if(issued>now||expires<=now||expires-now>60000||expires<=issued)throw std::runtime_error("RESET_COMMAND_EXPIRED");
    if(reset_pending())throw std::runtime_error("RESET_ALREADY_PENDING");
    object(after)["demoReset"]=Json{Json::Object{{"command",command},{"modelApplied",Json{false}},{"ack",Json{nullptr}},{"delivered",Json{false}}}};
    commit(before,after);
}
std::optional<std::string> AdvisoryRuntime::reset_model_command() const {
    const auto state=load();
    if(!state.object().count("demoReset")||state.at("demoReset").at("modelApplied").boolean()||
       !std::holds_alternative<std::nullptr_t>(state.at("demoReset").at("ack").value))return {};
    return state.at("demoReset").at("command").at("commandId").string();
}
void AdvisoryRuntime::reconcile_demo_reset(const v1::MessageMetadata& metadata_value) {
    const auto before=load();
    if(!reset_pending())return;
    const auto binding=parse_json(demo_poll(metadata_value));
    const auto& command=before.at("demoReset").at("command");
    bool matches=true;
    for(const auto& field:binding.object())if(encode_json(command.at(field.first))!=encode_json(field.second))matches=false;
    if(matches)return;
    auto after=before;auto& reset=object(object(after).at("demoReset"));
    auto ack=command.object();ack.erase("operation");ack.erase("issuedAt");ack.erase("expiresAt");
    ack["result"]=Json{std::string("REJECTED")};ack["clearRequest"]=Json{nullptr};ack["gatewayStatus"]=Json{nullptr};
    reset["ack"]=Json{ack};commit(before,after);
}
std::int64_t AdvisoryRuntime::reset_deadline() const {
    const auto state=load();if(!state.object().count("demoReset"))return 0;
    return v3::timestamp_milliseconds(state.at("demoReset").at("command").at("expiresAt").string());
}
void AdvisoryRuntime::model_reset_applied() {
    const auto before=load();auto after=before;
    if(!before.object().count("demoReset"))throw std::runtime_error("RESET_INTENT_MISSING");
    object(object(after).at("demoReset"))["modelApplied"]=Json{true};commit(before,after);
}
bool AdvisoryRuntime::reset_pending() const {
    const auto state=load();
    return state.object().count("demoReset")&&std::holds_alternative<std::nullptr_t>(state.at("demoReset").at("ack").value);
}
std::optional<std::string> AdvisoryRuntime::reset_ack(std::int64_t now) {
    const auto before=load();if(!before.object().count("demoReset"))return {};
    auto after=before;auto& reset=object(object(after).at("demoReset"));
    if(reset.at("delivered").boolean())return {};
    if(std::holds_alternative<std::nullptr_t>(reset.at("ack").value)) {
        if(now<v3::timestamp_milliseconds(reset.at("command").at("expiresAt").string()))return {};
        auto ack=reset.at("command").object();ack.erase("operation");ack.erase("issuedAt");ack.erase("expiresAt");
        ack["result"]=Json{std::string("FAILED")};ack["clearRequest"]=Json{nullptr};ack["gatewayStatus"]=Json{nullptr};
        reset["ack"]=Json{ack};commit(before,after);
    }
    return encode_json(reset.at("ack"));
}
void AdvisoryRuntime::reset_acknowledged(const std::string& bytes) {
    const auto before=load();if(!before.object().count("demoReset"))return;
    auto after=before;auto& reset=object(object(after).at("demoReset"));const auto response=parse_json(bytes,4096);
    if(std::holds_alternative<std::nullptr_t>(reset.at("ack").value)||response.object().size()!=3||
       response.at("schemaVersion").integer()!=1||response.at("commandId").string()!=reset.at("command").at("commandId").string()||
       response.at("state").string()!=reset.at("ack").at("result").string())throw std::runtime_error("RESET_ACK_INVALID");
    reset["delivered"]=Json{true};commit(before,after);
}
}  // namespace brake_health::runtime
