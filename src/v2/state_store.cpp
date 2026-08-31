// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0

#include "brake_health/v2/state_store.hpp"

#include "brake_health/v1/sha256.hpp"

#include <algorithm>
#include <charconv>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <tuple>
#include <unistd.h>

namespace brake_health::v2 {
namespace {

constexpr std::size_t kMaximumStateBytes = 65536U;
constexpr std::size_t kMaximumMessages = 64U;
constexpr std::size_t kMaximumOutboxBytes = 1048576U;

std::runtime_error posix_error(const std::string& operation) {
    return std::runtime_error(operation + ": " + std::strerror(errno));
}

void make_private_directory(const std::filesystem::path& path) {
    std::filesystem::create_directories(path);
    if (::chmod(path.c_str(), 0700) != 0) {
        throw posix_error("set private directory mode");
    }
}

std::string read_bounded(
    const std::filesystem::path& path,
    std::size_t maximum = kMaximumStateBytes) {
    struct stat status {};
    if (::lstat(path.c_str(), &status) != 0 || !S_ISREG(status.st_mode) ||
        (status.st_mode & 0777) != 0600) {
        throw std::runtime_error("required persistent file is absent");
    }
    const std::uintmax_t size = std::filesystem::file_size(path);
    if (size > maximum) {
        throw std::runtime_error("persistent file exceeds its bound");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot read persistent file");
    }
    std::string bytes(
        (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (input.bad()) {
        throw std::runtime_error("failed while reading persistent file");
    }
    return bytes;
}

std::size_t value_at(std::string_view json, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\":";
    const std::size_t at = json.find(needle);
    if (at == std::string_view::npos || json.find(needle, at + 1U) != std::string_view::npos) {
        throw std::runtime_error("persistent manifest field missing or duplicated");
    }
    return at + needle.size();
}

std::string string_value(std::string_view json, std::string_view key) {
    const std::size_t at = value_at(json, key);
    if (at >= json.size() || json[at] != '"') {
        throw std::runtime_error("persistent manifest string is malformed");
    }
    const std::size_t end = json.find('"', at + 1U);
    if (end == std::string_view::npos) {
        throw std::runtime_error("persistent manifest string is unterminated");
    }
    const std::string value(json.substr(at + 1U, end - at - 1U));
    if (value.find('\\') != std::string::npos) {
        throw std::runtime_error("persistent manifest escape is forbidden");
    }
    return value;
}

std::optional<std::string> optional_string_value(
    std::string_view json,
    std::string_view key) {
    const std::size_t at = value_at(json, key);
    if (json.substr(at, 4U) == "null") {
        return std::nullopt;
    }
    return string_value(json, key);
}

std::uint64_t unsigned_value(std::string_view json, std::string_view key) {
    const std::size_t at = value_at(json, key);
    const char* first = json.data() + at;
    const char* last = first;
    while (last != json.data() + json.size() && *last >= '0' && *last <= '9') ++last;
    if (first == last || (last - first > 1 && *first == '0')) {
        throw std::runtime_error("persistent manifest integer is malformed");
    }
    std::uint64_t value{};
    const auto result = std::from_chars(first, last, value);
    if (result.ec != std::errc{} || result.ptr != last) {
        throw std::runtime_error("persistent manifest integer is out of range");
    }
    return value;
}

std::vector<std::string> string_array(std::string_view json, std::string_view key) {
    std::size_t at = value_at(json, key);
    if (at >= json.size() || json[at++] != '[') {
        throw std::runtime_error("persistent manifest array is malformed");
    }
    std::vector<std::string> values;
    if (at < json.size() && json[at] == ']') return values;
    while (at < json.size()) {
        if (json[at++] != '"') throw std::runtime_error("persistent manifest item malformed");
        const std::size_t end = json.find('"', at);
        if (end == std::string_view::npos) {
            throw std::runtime_error("persistent manifest item unterminated");
        }
        values.emplace_back(json.substr(at, end - at));
        if (values.back().find('\n') != std::string::npos ||
            values.back().find('\\') != std::string::npos) {
            throw std::runtime_error("persistent manifest item escape forbidden");
        }
        at = end + 1U;
        if (at < json.size() && json[at] == ']') return values;
        if (at >= json.size() || json[at++] != ',') {
            throw std::runtime_error("persistent manifest array delimiter malformed");
        }
    }
    throw std::runtime_error("persistent manifest array unterminated");
}

struct OptionalEventFields {
    std::optional<std::string> id;
    std::optional<std::string> content_sha256;
    std::optional<std::string> idempotency_key_sha256;
    std::optional<std::string> message_sha256;
};

OptionalEventFields event_fields(std::string_view manifest) {
    OptionalEventFields fields{
        optional_string_value(manifest, "eventId"),
        optional_string_value(manifest, "eventContentSha256"),
        optional_string_value(manifest, "eventIdempotencyKeySha256"),
        optional_string_value(manifest, "eventMessageSha256")};
    const std::size_t count = static_cast<std::size_t>(fields.id.has_value()) +
                              static_cast<std::size_t>(fields.content_sha256.has_value()) +
                              static_cast<std::size_t>(fields.idempotency_key_sha256.has_value()) +
                              static_cast<std::size_t>(fields.message_sha256.has_value());
    if (count != 0U && count != 4U) {
        throw std::runtime_error("event manifest identity must be all present or all null");
    }
    return fields;
}

std::string message_sha(const CanonicalMessage& message) {
    return brake_health::v1::sha256_hex(message.canonical_json);
}

std::string manifest_json(
    const ModelState& before,
    const ModelState& after,
    std::string_view before_identities,
    std::string_view after_identities,
    const DerivedMessages& messages,
    bool admit) {
    const std::string before_bytes = state_json(before);
    const std::string after_bytes = state_json(after);
    const std::string event_content = messages.event
        ? "\"" + messages.event->content_sha256 + "\"" : "null";
    const std::string event_id = messages.event
        ? "\"" + messages.event->id + "\"" : "null";
    const std::string event_idempotency = messages.event
        ? "\"" + messages.event->idempotency_key_sha256 + "\"" : "null";
    const std::string event_message = messages.event
        ? "\"" + message_sha(*messages.event) + "\"" : "null";
    return std::string("{") +
        "\"afterSha256\":\"" + brake_health::v1::sha256_hex(after_bytes) + "\"" +
        ",\"afterIdentityLedgerSha256\":\"" +
        brake_health::v1::sha256_hex(after_identities) + "\"" +
        ",\"assessmentContentSha256\":\"" + messages.assessment.content_sha256 + "\"" +
        ",\"assessmentId\":\"" + messages.assessment.id + "\"" +
        ",\"assessmentIdempotencyKeySha256\":\"" +
        messages.assessment.idempotency_key_sha256 + "\"" +
        ",\"assessmentMessageSha256\":\"" + message_sha(messages.assessment) + "\"" +
        ",\"beforeSha256\":\"" + brake_health::v1::sha256_hex(before_bytes) + "\"" +
        ",\"beforeIdentityLedgerSha256\":\"" +
        brake_health::v1::sha256_hex(before_identities) + "\"" +
        ",\"disposition\":\"" + (admit ? "ADMIT" : "OVERFLOW_NOT_ENQUEUED") + "\"" +
        ",\"eventContentSha256\":" + event_content +
        ",\"eventId\":" + event_id +
        ",\"eventIdempotencyKeySha256\":" + event_idempotency +
        ",\"eventMessageSha256\":" + event_message + "}";
}

void verify_message(
    const std::filesystem::path& path,
    const std::string& expected_sha) {
    const std::string bytes = read_bounded(path, 16384U);
    if (brake_health::v1::sha256_hex(bytes) != expected_sha) {
        throw std::runtime_error("outbox message digest mismatch");
    }
}

OutboxEntry verified_outbox_message(
    std::string_view prefix,
    const std::string& manifest,
    const std::string& encoded,
    bool quarantined) {
    const bool assessment = prefix == "assessment";
    const std::string manifest_id = assessment
        ? string_value(manifest, "assessmentId") : *event_fields(manifest).id;
    const std::string message_type = assessment
        ? "BRAKE_HEALTH_ASSESSMENT" : "BRAKE_HEALTH_EVENT";
    const std::string content_sha = string_value(
        manifest, std::string(prefix) + "ContentSha256");
    const std::string idempotency_sha = string_value(
        manifest, std::string(prefix) + "IdempotencyKeySha256");
    const std::string encoded_sha = string_value(
        manifest, std::string(prefix) + "MessageSha256");
    const std::string unit_uid = string_value(encoded, "unitSystemUid");
    const std::string source_event_id = string_value(encoded, "sourceEventId");
    if (brake_health::v1::sha256_hex(encoded) != encoded_sha ||
        string_value(encoded, assessment ? "assessmentId" : "eventId") != manifest_id ||
        string_value(encoded, "messageType") != message_type ||
        string_value(encoded, "contentSha256") != content_sha ||
        string_value(encoded, "modelConfigSha256") != kModelConfigSha256 ||
        message_idempotency_key_sha256(unit_uid, message_type, manifest_id) !=
            idempotency_sha) {
        throw std::runtime_error("outbox message identity conflicts with bundle manifest");
    }
    const std::string assessment_id_value = assessment
        ? manifest_id : string_value(encoded, "assessmentId");
    if (assessment_id_value != string_value(manifest, "assessmentId")) {
        throw std::runtime_error("event assessment identity conflicts with its bundle");
    }
    return {
        manifest_id,
        message_type,
        encoded,
        content_sha,
        idempotency_sha,
        encoded_sha,
        source_event_id,
        assessment_id_value,
        quarantined};
}

bool dot_name(const std::filesystem::path& path) {
    const std::string name = path.filename().string();
    return !name.empty() && name.front() == '.';
}

bool lowercase_sha256(std::string_view value) {
    return value.size() == 64U && std::all_of(value.begin(), value.end(), [](char character) {
        return (character >= '0' && character <= '9') ||
               (character >= 'a' && character <= 'f');
    });
}

bool uuid_version(std::string_view value, char version) {
    if (value.size() != 36U || value[8] != '-' || value[13] != '-' ||
        value[18] != '-' || value[23] != '-' || value[14] != version ||
        std::string_view("89ab").find(value[19]) == std::string_view::npos) return false;
    for (std::size_t index = 0U; index < value.size(); ++index) {
        if (index == 8U || index == 13U || index == 18U || index == 23U) continue;
        if (!((value[index] >= '0' && value[index] <= '9') ||
              (value[index] >= 'a' && value[index] <= 'f'))) return false;
    }
    return true;
}

}  // namespace

bool derived_outbox_admissible(
    std::size_t current_count,
    std::size_t current_bytes,
    std::size_t incoming_count,
    std::size_t incoming_bytes) noexcept {
    return current_count <= kMaximumMessages && incoming_count <= kMaximumMessages - current_count &&
           current_bytes <= kMaximumOutboxBytes &&
           incoming_bytes <= kMaximumOutboxBytes - current_bytes;
}

template <typename Bindings>
std::string identity_bindings_json(const Bindings& bindings) {
    std::string entries = "[";
    for (std::size_t index = 0U; index < bindings.size(); ++index) {
        if (index != 0U) entries.push_back(',');
        entries += "\"" + bindings[index].source_event_id + "=" +
                   bindings[index].assessment_id + "\"";
    }
    entries.push_back(']');
    return entries;
}

std::string StateStore::identity_ledger_json(const IdentityLedger& ledger) {
    const std::string entries = identity_bindings_json(ledger.entries);
    return std::string("{") +
        "\"bindingsSha256\":\"" + brake_health::v1::sha256_hex(entries) + "\"" +
        ",\"entries\":" + entries +
        ",\"generation\":" + std::to_string(ledger.generation) +
        ",\"schemaVersion\":1" +
        ",\"stateSha256\":\"" + ledger.state_sha256 + "\"}";
}

StateStore::IdentityLedger StateStore::parse_identity_ledger(std::string_view bytes) {
    if (bytes.size() > kMaximumStateBytes || unsigned_value(bytes, "schemaVersion") != 1U) {
        throw std::runtime_error("identity ledger schema is invalid");
    }
    IdentityLedger ledger;
    ledger.generation = unsigned_value(bytes, "generation");
    ledger.state_sha256 = string_value(bytes, "stateSha256");
    const std::string bindings_sha256 = string_value(bytes, "bindingsSha256");
    if (!lowercase_sha256(ledger.state_sha256) ||
        !lowercase_sha256(bindings_sha256)) {
        throw std::runtime_error("identity ledger state digest is malformed");
    }
    for (const std::string& value : string_array(bytes, "entries")) {
        const std::size_t separator = value.find('=');
        if (separator == std::string::npos || value.find('=', separator + 1U) != std::string::npos) {
            throw std::runtime_error("identity ledger binding is malformed");
        }
        IdentityBinding binding{value.substr(0U, separator), value.substr(separator + 1U)};
        if (!uuid_version(binding.source_event_id, '4') ||
            !uuid_version(binding.assessment_id, '5')) {
            throw std::runtime_error("identity ledger UUID is malformed");
        }
        ledger.entries.push_back(std::move(binding));
    }
    if (ledger.entries.size() > 64U ||
        brake_health::v1::sha256_hex(identity_bindings_json(ledger.entries)) !=
            bindings_sha256 ||
        identity_ledger_json(ledger) != bytes) {
        throw std::runtime_error("identity ledger is not canonical or exceeds its bound");
    }
    return ledger;
}

void StateStore::validate_identity_ledger(
    const IdentityLedger& ledger,
    const ModelState& state,
    std::string_view state_bytes) {
    if (ledger.generation != state.generation ||
        ledger.state_sha256 != brake_health::v1::sha256_hex(state_bytes) ||
        ledger.entries.size() != state.recent_source_event_ids.size()) {
        throw std::runtime_error("identity ledger does not bind the current state");
    }
    for (std::size_t index = 0U; index < ledger.entries.size(); ++index) {
        if (ledger.entries[index].source_event_id != state.recent_source_event_ids[index]) {
            throw std::runtime_error("identity ledger source order conflicts with state");
        }
    }
    std::set<std::string> assessment_ids;
    for (const IdentityBinding& binding : ledger.entries) {
        if (!assessment_ids.insert(binding.assessment_id).second) {
            throw std::runtime_error("identity ledger assessment identity is duplicated");
        }
    }
    if (!ledger.entries.empty() &&
        (!state.last_assessment_id ||
         ledger.entries.back().assessment_id != *state.last_assessment_id)) {
        throw std::runtime_error("identity ledger last assessment conflicts with state");
    }
}

StateStore::IdentityLedger StateStore::identity_ledger(const ModelState& state) const {
    const IdentityLedger ledger = parse_identity_ledger(
        read_bounded(state_root_ / "identity-ledger.json"));
    validate_identity_ledger(ledger, state, state_json(state));
    return ledger;
}

StateStore::StateStore(
    std::filesystem::path state_root,
    std::filesystem::path outbox_root,
    std::string producer_epoch,
    FaultInjector fault_injector)
    : state_root_(std::move(state_root)),
      outbox_root_(std::move(outbox_root)),
      producer_epoch_(std::move(producer_epoch)),
      fault_injector_(std::move(fault_injector)) {
    try {
        static_cast<void>(initial_state(producer_epoch_));
        make_private_directory(state_root_);
        make_private_directory(state_root_ / "transactions");
        make_private_directory(outbox_root_);
        const auto remove_stale = [&](const std::filesystem::path& parent) {
            bool changed = false;
            for (const auto& entry : std::filesystem::directory_iterator(parent)) {
                const std::string name = entry.path().filename().string();
                if (name.rfind(".staging-", 0U) == 0U ||
                    name.rfind(".recovery-", 0U) == 0U ||
                    (!name.empty() && name.front() == '.' &&
                     name.find(".tmp-") != std::string::npos)) {
                    std::filesystem::remove_all(entry.path());
                    changed = true;
                }
            }
            if (changed) sync_directory(parent);
        };
        remove_stale(state_root_);
        remove_stale(state_root_ / "transactions");
        remove_stale(outbox_root_);
        for (const auto& entry : std::filesystem::directory_iterator(
                 state_root_ / "transactions")) {
            if (entry.is_directory() && !dot_name(entry.path())) remove_stale(entry.path());
        }
        for (const auto& entry : std::filesystem::directory_iterator(outbox_root_)) {
            if (entry.is_directory() && !dot_name(entry.path())) remove_stale(entry.path());
        }
        if (!std::filesystem::exists(state_root_ / "state.json")) {
            bool has_transaction = false;
            for (const auto& entry : std::filesystem::directory_iterator(
                     state_root_ / "transactions")) {
                has_transaction = has_transaction || !dot_name(entry.path());
            }
            if (has_transaction) {
                throw std::runtime_error("state is absent while a journal exists");
            }
            const std::string initial_bytes = state_json(initial_state(producer_epoch_));
            atomic_write(state_root_ / "state.json", initial_bytes);
            const IdentityLedger initial_identities{
                0U, brake_health::v1::sha256_hex(initial_bytes), {}};
            atomic_write(
                state_root_ / "identity-ledger.json",
                identity_ledger_json(initial_identities));
        }
        if (!std::filesystem::exists(state_root_ / "identity-ledger.json")) {
            const std::string existing_state = read_bounded(state_root_ / "state.json");
            const ModelState parsed = parse_state_json(existing_state);
            if (parsed.generation != 0U || !parsed.recent_source_event_ids.empty()) {
                throw std::runtime_error("committed identity ledger is absent");
            }
            atomic_write(
                state_root_ / "identity-ledger.json",
                identity_ledger_json({
                    0U, brake_health::v1::sha256_hex(existing_state), {}}));
        }
        recover();
        if (state().producer_epoch != producer_epoch_) {
            throw std::runtime_error("persistent producer epoch does not match this instance");
        }
        static_cast<void>(inventory());
    } catch (...) {
        ready_ = false;
    }
}

void StateStore::sync_directory(const std::filesystem::path& directory) const {
    const int descriptor = ::open(directory.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        throw posix_error("open directory for synchronization");
    }
    if (::fsync(descriptor) != 0) {
        const int saved = errno;
        ::close(descriptor);
        errno = saved;
        throw posix_error("synchronize directory");
    }
    if (::close(descriptor) != 0) {
        throw posix_error("close synchronized directory");
    }
}

void StateStore::atomic_write(
    const std::filesystem::path& target,
    std::string_view bytes) const {
    make_private_directory(target.parent_path());
    const std::filesystem::path temporary = target.parent_path() /
        ("." + target.filename().string() + ".tmp-" + std::to_string(::getpid()) + "-" +
         std::to_string(temporary_counter_++));
    const int descriptor = ::open(
        temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        throw posix_error("create temporary persistent file");
    }
    try {
        std::size_t written = 0U;
        while (written < bytes.size()) {
            const ssize_t count = ::write(
                descriptor, bytes.data() + written, bytes.size() - written);
            if (count < 0) {
                if (errno == EINTR) continue;
                throw posix_error("write temporary persistent file");
            }
            if (count == 0) {
                throw std::runtime_error("zero-length persistent write");
            }
            written += static_cast<std::size_t>(count);
        }
        if (::fsync(descriptor) != 0) {
            throw posix_error("synchronize temporary persistent file");
        }
        if (::close(descriptor) != 0) {
            throw posix_error("close temporary persistent file");
        }
        if (::rename(temporary.c_str(), target.c_str()) != 0) {
            throw posix_error("atomically replace persistent file");
        }
        sync_directory(target.parent_path());
    } catch (...) {
        const int saved = errno;
        ::close(descriptor);
        std::filesystem::remove(temporary);
        errno = saved;
        throw;
    }
}

void StateStore::fail_if_requested(WriteStage stage) const {
    if (fault_injector_ && fault_injector_(stage)) {
        throw std::runtime_error("injected v2 persistence interruption");
    }
}

void StateStore::persist_transaction(
    const ModelState& before,
    const ModelState& after,
    const IdentityLedger& before_identities,
    const IdentityLedger& after_identities,
    const DerivedMessages& messages,
    bool admit) {
    const std::string before_identity_bytes = identity_ledger_json(before_identities);
    const std::string after_identity_bytes = identity_ledger_json(after_identities);
    const std::filesystem::path transactions = state_root_ / "transactions";
    const std::filesystem::path journal = transactions / messages.assessment.id;
    if (std::filesystem::exists(journal)) {
        throw std::runtime_error("assessment transaction already exists");
    }
    const std::filesystem::path staging = transactions /
        (".staging-" + messages.assessment.id + "-" +
         std::to_string(temporary_counter_++));
    make_private_directory(staging);
    atomic_write(staging / "before.json", state_json(before));
    atomic_write(staging / "after.json", state_json(after));
    atomic_write(staging / "before-identities.json", before_identity_bytes);
    atomic_write(staging / "after-identities.json", after_identity_bytes);
    atomic_write(staging / "assessment.json", messages.assessment.canonical_json);
    if (messages.event) {
        atomic_write(staging / "event.json", messages.event->canonical_json);
    }
    atomic_write(
        staging / "manifest.json",
        manifest_json(
            before, after, before_identity_bytes, after_identity_bytes, messages, admit));
    sync_directory(staging);
    fail_if_requested(WriteStage::JournalFiles);
    if (::rename(staging.c_str(), journal.c_str()) != 0) {
        throw posix_error("publish immutable transaction journal");
    }
    sync_directory(transactions);
    fail_if_requested(WriteStage::Journal);

    atomic_write(state_root_ / "state.json", state_json(after));
    fail_if_requested(WriteStage::State);
    atomic_write(state_root_ / "identity-ledger.json", after_identity_bytes);
    fail_if_requested(WriteStage::IdentityLedger);

    if (admit) {
        const std::filesystem::path bundle = outbox_root_ / messages.assessment.id;
        const std::filesystem::path bundle_staging = outbox_root_ /
            (".staging-" + messages.assessment.id + "-" +
             std::to_string(temporary_counter_++));
        make_private_directory(bundle_staging);
        atomic_write(bundle_staging / "assessment.json", messages.assessment.canonical_json);
        if (messages.event) {
            atomic_write(bundle_staging / "event.json", messages.event->canonical_json);
        }
        atomic_write(
            bundle_staging / "manifest.json",
            manifest_json(
                before, after, before_identity_bytes, after_identity_bytes, messages, true));
        sync_directory(bundle_staging);
        fail_if_requested(WriteStage::BundleFiles);
        if (::rename(bundle_staging.c_str(), bundle.c_str()) != 0) {
            throw posix_error("publish atomic derived-message bundle");
        }
        sync_directory(outbox_root_);
        fail_if_requested(WriteStage::Bundle);
    }

    atomic_write(journal / "committed", "COMMITTED\n");
    fail_if_requested(WriteStage::CommitMarker);
    const std::string expected_manifest = manifest_json(
        before, after, before_identity_bytes, after_identity_bytes, messages, admit);
    if (read_bounded(journal / "before.json") != state_json(before) ||
        read_bounded(journal / "after.json") != state_json(after) ||
        read_bounded(journal / "before-identities.json") != before_identity_bytes ||
        read_bounded(journal / "after-identities.json") != after_identity_bytes ||
        read_bounded(journal / "assessment.json", 16384U) !=
            messages.assessment.canonical_json ||
        read_bounded(journal / "manifest.json") != expected_manifest ||
        read_bounded(journal / "committed") != "COMMITTED\n" ||
        read_bounded(state_root_ / "state.json") != state_json(after) ||
        read_bounded(state_root_ / "identity-ledger.json") != after_identity_bytes) {
        throw std::runtime_error("transaction changed before journal removal");
    }
    if (messages.event) {
        if (read_bounded(journal / "event.json", 16384U) != messages.event->canonical_json) {
            throw std::runtime_error("transaction event changed before journal removal");
        }
    } else if (std::filesystem::exists(journal / "event.json")) {
        throw std::runtime_error("unexpected transaction event before journal removal");
    }
    const std::filesystem::path admitted_bundle = outbox_root_ / messages.assessment.id;
    if (admit) {
        if (read_bounded(admitted_bundle / "manifest.json") != expected_manifest ||
            read_bounded(admitted_bundle / "assessment.json", 16384U) !=
                messages.assessment.canonical_json ||
            (messages.event &&
             read_bounded(admitted_bundle / "event.json", 16384U) !=
                 messages.event->canonical_json)) {
            throw std::runtime_error("admitted bundle changed before journal removal");
        }
    } else if (std::filesystem::exists(admitted_bundle)) {
        throw std::runtime_error("overflow disposition unexpectedly published a bundle");
    }
    fail_if_requested(WriteStage::JournalRemoval);
    std::filesystem::remove_all(journal);
    sync_directory(transactions);
}

void StateStore::recover() {
    const std::filesystem::path transactions = state_root_ / "transactions";
    std::vector<std::filesystem::path> journals;
    for (const auto& entry : std::filesystem::directory_iterator(transactions)) {
        if (!dot_name(entry.path())) {
            journals.push_back(entry.path());
        }
    }
    std::sort(journals.begin(), journals.end());
    for (const std::filesystem::path& journal : journals) {
        if (!std::filesystem::is_directory(journal) ||
            std::filesystem::exists(journal / "QUARANTINED")) {
            ready_ = false;
            return;
        }
        try {
            for (const auto& entry : std::filesystem::directory_iterator(journal)) {
                const std::string name = entry.path().filename().string();
                if (name != "before.json" && name != "after.json" &&
                    name != "before-identities.json" && name != "after-identities.json" &&
                    name != "assessment.json" && name != "event.json" &&
                    name != "manifest.json" && name != "committed" &&
                    name != "QUARANTINED") {
                    throw std::runtime_error("unknown transaction journal file");
                }
            }
            const std::string before = read_bounded(journal / "before.json");
            const std::string after = read_bounded(journal / "after.json");
            const ModelState before_state = parse_state_json(before);
            const ModelState after_state = parse_state_json(after);
            const std::string before_identity_bytes =
                read_bounded(journal / "before-identities.json");
            const std::string after_identity_bytes =
                read_bounded(journal / "after-identities.json");
            const IdentityLedger before_identities =
                parse_identity_ledger(before_identity_bytes);
            const IdentityLedger after_identities =
                parse_identity_ledger(after_identity_bytes);
            validate_identity_ledger(before_identities, before_state, before);
            validate_identity_ledger(after_identities, after_state, after);
            const std::string manifest = read_bounded(journal / "manifest.json");
            if (brake_health::v1::sha256_hex(before) != string_value(manifest, "beforeSha256") ||
                brake_health::v1::sha256_hex(after) != string_value(manifest, "afterSha256") ||
                brake_health::v1::sha256_hex(before_identity_bytes) !=
                    string_value(manifest, "beforeIdentityLedgerSha256") ||
                brake_health::v1::sha256_hex(after_identity_bytes) !=
                    string_value(manifest, "afterIdentityLedgerSha256")) {
                throw std::runtime_error("journal state digest mismatch");
            }
            const std::string assessment = read_bounded(journal / "assessment.json", 16384U);
            if (brake_health::v1::sha256_hex(assessment) !=
                string_value(manifest, "assessmentMessageSha256")) {
                throw std::runtime_error("journal assessment digest mismatch");
            }
            static_cast<void>(verified_outbox_message(
                "assessment", manifest, assessment, false));
            const OptionalEventFields manifest_event = event_fields(manifest);
            std::optional<std::string> event;
            if (manifest_event.id) {
                event = read_bounded(journal / "event.json", 16384U);
                if (brake_health::v1::sha256_hex(*event) !=
                    *manifest_event.message_sha256) {
                    throw std::runtime_error("journal event digest mismatch");
                }
                static_cast<void>(verified_outbox_message("event", manifest, *event, false));
            } else if (std::filesystem::exists(journal / "event.json")) {
                throw std::runtime_error("unexpected journal event");
            }

            const std::string disposition = string_value(manifest, "disposition");
            if (disposition != "ADMIT" && disposition != "OVERFLOW_NOT_ENQUEUED") {
                throw std::runtime_error("unknown transaction disposition");
            }
            const std::string current = read_bounded(state_root_ / "state.json");
            const std::string current_identities =
                read_bounded(state_root_ / "identity-ledger.json");
            if (current == before) {
                atomic_write(state_root_ / "state.json", after);
            } else if (current != after) {
                throw std::runtime_error("journal generation or state conflict");
            }
            if (current_identities == before_identity_bytes) {
                atomic_write(state_root_ / "identity-ledger.json", after_identity_bytes);
            } else if (current_identities != after_identity_bytes) {
                throw std::runtime_error("journal identity-ledger conflict");
            }

            if (disposition == "ADMIT") {
                const std::string id = string_value(manifest, "assessmentId");
                const std::filesystem::path bundle = outbox_root_ / id;
                if (!std::filesystem::exists(bundle)) {
                    const std::filesystem::path staging = outbox_root_ /
                        (".recovery-" + id + "-" + std::to_string(temporary_counter_++));
                    make_private_directory(staging);
                    atomic_write(staging / "assessment.json", assessment);
                    if (event) atomic_write(staging / "event.json", *event);
                    atomic_write(staging / "manifest.json", manifest);
                    sync_directory(staging);
                    if (::rename(staging.c_str(), bundle.c_str()) != 0) {
                        throw posix_error("recover atomic derived-message bundle");
                    }
                    sync_directory(outbox_root_);
                }
                if (read_bounded(bundle / "manifest.json") != manifest) {
                    throw std::runtime_error("outbox bundle manifest conflicts with journal");
                }
                verify_message(
                    bundle / "assessment.json",
                    string_value(manifest, "assessmentMessageSha256"));
                if (event) {
                    verify_message(
                        bundle / "event.json",
                        *manifest_event.message_sha256);
                } else if (std::filesystem::exists(bundle / "event.json")) {
                    throw std::runtime_error("unexpected event in recovered assessment-only bundle");
                }
            }
            if (!std::filesystem::exists(journal / "committed")) {
                atomic_write(journal / "committed", "COMMITTED\n");
            }
            if (read_bounded(journal / "before.json") != before ||
                read_bounded(journal / "after.json") != after ||
                read_bounded(journal / "before-identities.json") != before_identity_bytes ||
                read_bounded(journal / "after-identities.json") != after_identity_bytes ||
                read_bounded(journal / "assessment.json", 16384U) != assessment ||
                read_bounded(journal / "manifest.json") != manifest ||
                read_bounded(journal / "committed") != "COMMITTED\n" ||
                read_bounded(state_root_ / "state.json") != after ||
                read_bounded(state_root_ / "identity-ledger.json") !=
                    after_identity_bytes) {
                throw std::runtime_error("recovered transaction changed before journal removal");
            }
            const std::filesystem::path expected_bundle =
                outbox_root_ / string_value(manifest, "assessmentId");
            if (disposition == "ADMIT") {
                if (read_bounded(expected_bundle / "manifest.json") != manifest ||
                    read_bounded(expected_bundle / "assessment.json", 16384U) != assessment ||
                    (event && read_bounded(expected_bundle / "event.json", 16384U) != *event)) {
                    throw std::runtime_error(
                        "recovered admitted bundle changed before journal removal");
                }
            } else if (std::filesystem::exists(expected_bundle)) {
                throw std::runtime_error(
                    "recovered overflow disposition unexpectedly has a bundle");
            }
            std::filesystem::remove_all(journal);
            sync_directory(transactions);
        } catch (...) {
            try {
                atomic_write(journal / "QUARANTINED", "NOT_READY_STATE\n");
            } catch (...) {
            }
            ready_ = false;
            return;
        }
    }
}

std::vector<OutboxEntry> StateStore::inventory_verified() const {
    std::vector<OutboxEntry> result;
    std::size_t bytes = 0U;
    std::map<std::string, std::tuple<std::string, std::string, std::string>> identities;
    for (const auto& entry : std::filesystem::directory_iterator(outbox_root_)) {
        if (dot_name(entry.path())) continue;
        const std::filesystem::path bundle = entry.path();
        try {
            if (!entry.is_directory()) {
                throw std::runtime_error("unexpected outbox entry");
            }
            for (const auto& file : std::filesystem::directory_iterator(bundle)) {
                const std::string name = file.path().filename().string();
                if (name != "assessment.json" && name != "event.json" &&
                    name != "manifest.json" && name != "assessment.ack" &&
                    name != "event.ack" && name != "QUARANTINED") {
                    throw std::runtime_error("unknown outbox bundle file");
                }
            }
            const std::string manifest = read_bounded(bundle / "manifest.json");
            if (bundle.filename() != string_value(manifest, "assessmentId")) {
                throw std::runtime_error("outbox bundle directory identity mismatch");
            }
            if (string_value(manifest, "disposition") != "ADMIT") {
                throw std::runtime_error("outbox bundle does not have an admitted disposition");
            }
            const OptionalEventFields manifest_event = event_fields(manifest);
            const bool quarantined = std::filesystem::exists(bundle / "QUARANTINED");
            const auto append = [&](std::string_view prefix, const std::filesystem::path& path) {
                if (!std::filesystem::exists(path)) return;
                const std::string encoded = read_bounded(path, 16384U);
                OutboxEntry value = verified_outbox_message(
                    prefix, manifest, encoded, quarantined);
                const auto identity = std::make_tuple(
                    value.content_sha256,
                    value.idempotency_key_sha256,
                    value.message_sha256);
                const auto [found, inserted] = identities.emplace(value.id, identity);
                if (!inserted) {
                    if (found->second != identity) {
                        throw std::runtime_error(
                            "same outbox identity has incompatible committed digests");
                    }
                    throw std::runtime_error("duplicate committed outbox identity");
                }
                bytes += encoded.size();
                result.push_back(std::move(value));
                if (result.size() > kMaximumMessages || bytes > kMaximumOutboxBytes) {
                    throw std::runtime_error("persisted outbox exceeds its accepted bounds");
                }
            };
            append("assessment", bundle / "assessment.json");
            if (manifest_event.id) {
                append("event", bundle / "event.json");
            } else if (std::filesystem::exists(bundle / "event.json")) {
                throw std::runtime_error("unexpected event in assessment-only bundle");
            }
        } catch (...) {
            try {
                if (std::filesystem::is_directory(bundle)) {
                    atomic_write(bundle / "QUARANTINED", "NOT_READY_STATE\n");
                }
            } catch (...) {
            }
            throw;
        }
    }
    std::sort(result.begin(), result.end(), [](const OutboxEntry& left, const OutboxEntry& right) {
        return left.id < right.id;
    });
    return result;
}

std::vector<OutboxEntry> StateStore::inventory() const {
    if (!ready_) return {};
    try {
        return inventory_verified();
    } catch (...) {
        ready_ = false;
        return {};
    }
}

ModelState StateStore::state() const {
    const ModelState current = parse_state_json(read_bounded(state_root_ / "state.json"));
    static_cast<void>(identity_ledger(current));
    return current;
}

ProcessResult StateStore::process(
    const CompletedEpisode& episode,
    const DeploymentMetadata& metadata,
    const SyntheticModel& model) {
    if (!ready_) {
        return {ProcessStatus::NotReadyState, std::nullopt, std::nullopt, false};
    }
    try {
        const ModelState before = state();
        const IdentityLedger before_identities = identity_ledger(before);
        const std::vector<OutboxEntry> current = inventory();
        if (!ready_) {
            return {ProcessStatus::NotReadyState, std::nullopt, std::nullopt, false};
        }
        const auto duplicate = std::find(
            before.recent_source_event_ids.begin(),
            before.recent_source_event_ids.end(),
            episode.source_event_id);
        if (duplicate != before.recent_source_event_ids.end()) {
            const std::size_t duplicate_index = static_cast<std::size_t>(
                std::distance(before.recent_source_event_ids.begin(), duplicate));
            const std::string& committed_id =
                before_identities.entries.at(duplicate_index).assessment_id;
            std::set<std::string> stored_assessment_ids;
            for (const OutboxEntry& entry : current) {
                if (entry.source_event_id == episode.source_event_id) {
                    stored_assessment_ids.insert(entry.assessment_id);
                }
            }
            if (stored_assessment_ids.size() > 1U ||
                (!stored_assessment_ids.empty() &&
                 *stored_assessment_ids.begin() != committed_id)) {
                ready_ = false;
                return {ProcessStatus::NotReadyState, std::nullopt, std::nullopt, false};
            }
            return {ProcessStatus::Duplicate, std::nullopt, committed_id, false};
        }

        Evaluation evaluation = model.evaluate(episode, before);
        if (evaluation.skip_reason) {
            return {
                ProcessStatus::SkippedInputQuality,
                evaluation.skip_reason,
                std::nullopt,
                false};
        }
        if (!evaluation.assessment) {
            ready_ = false;
            return {ProcessStatus::NotReadyState, std::nullopt, std::nullopt, false};
        }
        DerivedMessages messages = build_messages(metadata, episode, *evaluation.assessment);
        ModelState after = evaluation.next_state;
        after.last_applied_source_event_id = episode.source_event_id;
        after.last_assessment_id = messages.assessment.id;
        after.recent_source_event_ids.push_back(episode.source_event_id);
        if (after.recent_source_event_ids.size() > 64U) {
            after.recent_source_event_ids.erase(after.recent_source_event_ids.begin());
        }
        static_cast<void>(state_json(after));
        IdentityLedger after_identities = before_identities;
        after_identities.generation = after.generation;
        after_identities.entries.push_back(
            {episode.source_event_id, messages.assessment.id});
        if (after_identities.entries.size() > 64U) {
            after_identities.entries.erase(after_identities.entries.begin());
        }
        after_identities.state_sha256 =
            brake_health::v1::sha256_hex(state_json(after));
        validate_identity_ledger(after_identities, after, state_json(after));

        std::size_t current_bytes = 0U;
        for (const OutboxEntry& value : current) {
            current_bytes += value.canonical_json.size();
        }
        const bool admit = derived_outbox_admissible(
            current.size(), current_bytes, messages.count(), messages.encoded_bytes());
        try {
            persist_transaction(
                before,
                after,
                before_identities,
                after_identities,
                messages,
                admit);
        } catch (...) {
            ready_ = false;
            throw;
        }
        return {
            admit ? ProcessStatus::Produced : ProcessStatus::DerivedOutboxFull,
            std::nullopt,
            messages.assessment.id,
            admit && messages.event.has_value()};
    } catch (const std::invalid_argument&) {
        ready_ = false;
        return {ProcessStatus::NotReadyState, std::nullopt, std::nullopt, false};
    }
}

bool StateStore::acknowledge(
    const std::string& id,
    const std::string& idempotency_key_sha256,
    const std::string& content_sha256) {
    if (!ready_) return false;
    try {
        const std::vector<OutboxEntry> entries = inventory();
        const auto found = std::find_if(entries.begin(), entries.end(), [&](const OutboxEntry& value) {
            return value.id == id;
        });
        if (found == entries.end()) return false;
        if (found->quarantined) return false;
        std::filesystem::path actual_bundle;
        for (const auto& entry : std::filesystem::directory_iterator(outbox_root_)) {
            if (dot_name(entry.path()) || !entry.is_directory()) continue;
            const std::string manifest = read_bounded(entry.path() / "manifest.json");
            const auto event = optional_string_value(manifest, "eventId");
            if (string_value(manifest, "assessmentId") == id || (event && *event == id)) {
                actual_bundle = entry.path();
                break;
            }
        }
        if (actual_bundle.empty() || !std::filesystem::exists(actual_bundle)) {
            throw std::runtime_error("outbox bundle disappeared during acknowledgement");
        }
        if (found->idempotency_key_sha256 != idempotency_key_sha256 ||
            found->content_sha256 != content_sha256) {
            atomic_write(actual_bundle / "QUARANTINED", "ACK_DIGEST_CONFLICT\n");
            return false;
        }
        const bool assessment = found->message_type == "BRAKE_HEALTH_ASSESSMENT";
        const std::filesystem::path message = actual_bundle /
            (assessment ? "assessment.json" : "event.json");
        atomic_write(
            actual_bundle / (assessment ? "assessment.ack" : "event.ack"),
            id + "\n" + idempotency_key_sha256 + "\n" + content_sha256 + "\n");
        if (!std::filesystem::remove(message)) {
            throw std::runtime_error("acknowledged message was not deleted");
        }
        sync_directory(actual_bundle);
        if (!std::filesystem::exists(actual_bundle / "assessment.json") &&
            !std::filesystem::exists(actual_bundle / "event.json")) {
            std::filesystem::remove_all(actual_bundle);
            sync_directory(outbox_root_);
        }
        return true;
    } catch (...) {
        ready_ = false;
        return false;
    }
}

}  // namespace brake_health::v2
