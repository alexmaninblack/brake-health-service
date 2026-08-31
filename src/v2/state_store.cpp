// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0

#include "brake_health/v2/state_store.hpp"

#include "brake_health/v1/sha256.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
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

std::string message_sha(const CanonicalMessage& message) {
    return brake_health::v1::sha256_hex(message.canonical_json);
}

std::string manifest_json(
    const ModelState& before,
    const ModelState& after,
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
        ",\"assessmentContentSha256\":\"" + messages.assessment.content_sha256 + "\"" +
        ",\"assessmentId\":\"" + messages.assessment.id + "\"" +
        ",\"assessmentIdempotencyKeySha256\":\"" +
        messages.assessment.idempotency_key_sha256 + "\"" +
        ",\"assessmentMessageSha256\":\"" + message_sha(messages.assessment) + "\"" +
        ",\"beforeSha256\":\"" + brake_health::v1::sha256_hex(before_bytes) + "\"" +
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

bool dot_name(const std::filesystem::path& path) {
    const std::string name = path.filename().string();
    return !name.empty() && name.front() == '.';
}

}  // namespace

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
            atomic_write(state_root_ / "state.json", state_json(initial_state(producer_epoch_)));
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
    const DerivedMessages& messages,
    bool admit) {
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
    atomic_write(staging / "assessment.json", messages.assessment.canonical_json);
    if (messages.event) {
        atomic_write(staging / "event.json", messages.event->canonical_json);
    }
    atomic_write(staging / "manifest.json", manifest_json(before, after, messages, admit));
    sync_directory(staging);
    if (::rename(staging.c_str(), journal.c_str()) != 0) {
        throw posix_error("publish immutable transaction journal");
    }
    sync_directory(transactions);
    fail_if_requested(WriteStage::Journal);

    atomic_write(state_root_ / "state.json", state_json(after));
    fail_if_requested(WriteStage::State);

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
        atomic_write(bundle_staging / "manifest.json", manifest_json(before, after, messages, true));
        sync_directory(bundle_staging);
        if (::rename(bundle_staging.c_str(), bundle.c_str()) != 0) {
            throw posix_error("publish atomic derived-message bundle");
        }
        sync_directory(outbox_root_);
        fail_if_requested(WriteStage::Bundle);
    }

    atomic_write(journal / "committed", "COMMITTED\n");
    fail_if_requested(WriteStage::CommitMarker);
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
                    name != "assessment.json" && name != "event.json" &&
                    name != "manifest.json" && name != "committed" &&
                    name != "QUARANTINED") {
                    throw std::runtime_error("unknown transaction journal file");
                }
            }
            const std::string before = read_bounded(journal / "before.json");
            const std::string after = read_bounded(journal / "after.json");
            static_cast<void>(parse_state_json(before));
            static_cast<void>(parse_state_json(after));
            const std::string manifest = read_bounded(journal / "manifest.json");
            if (brake_health::v1::sha256_hex(before) != string_value(manifest, "beforeSha256") ||
                brake_health::v1::sha256_hex(after) != string_value(manifest, "afterSha256")) {
                throw std::runtime_error("journal state digest mismatch");
            }
            const std::string assessment = read_bounded(journal / "assessment.json", 16384U);
            if (brake_health::v1::sha256_hex(assessment) !=
                string_value(manifest, "assessmentMessageSha256")) {
                throw std::runtime_error("journal assessment digest mismatch");
            }
            const std::optional<std::string> event_id = optional_string_value(manifest, "eventId");
            std::optional<std::string> event;
            if (event_id) {
                event = read_bounded(journal / "event.json", 16384U);
                if (brake_health::v1::sha256_hex(*event) !=
                    *optional_string_value(manifest, "eventMessageSha256")) {
                    throw std::runtime_error("journal event digest mismatch");
                }
            } else if (std::filesystem::exists(journal / "event.json")) {
                throw std::runtime_error("unexpected journal event");
            }

            const std::string disposition = string_value(manifest, "disposition");
            if (disposition != "ADMIT" && disposition != "OVERFLOW_NOT_ENQUEUED") {
                throw std::runtime_error("unknown transaction disposition");
            }
            const std::string current = read_bounded(state_root_ / "state.json");
            if (current == before) {
                atomic_write(state_root_ / "state.json", after);
            } else if (current != after) {
                throw std::runtime_error("journal generation or state conflict");
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
                        *optional_string_value(manifest, "eventMessageSha256"));
                } else if (std::filesystem::exists(bundle / "event.json")) {
                    throw std::runtime_error("unexpected event in recovered assessment-only bundle");
                }
            }
            if (!std::filesystem::exists(journal / "committed")) {
                atomic_write(journal / "committed", "COMMITTED\n");
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

std::vector<OutboxEntry> StateStore::inventory() const {
    std::vector<OutboxEntry> result;
    std::size_t bytes = 0U;
    for (const auto& entry : std::filesystem::directory_iterator(outbox_root_)) {
        if (dot_name(entry.path())) continue;
        if (!entry.is_directory()) {
            throw std::runtime_error("unexpected outbox entry");
        }
        const std::filesystem::path bundle = entry.path();
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
        const bool quarantined = std::filesystem::exists(bundle / "QUARANTINED");
        const auto append = [&](std::string_view prefix, const std::filesystem::path& path) {
            if (!std::filesystem::exists(path)) return;
            const std::string encoded = read_bounded(path, 16384U);
            const std::string expected_message = string_value(
                manifest, std::string(prefix) + "MessageSha256");
            if (brake_health::v1::sha256_hex(encoded) != expected_message) {
                throw std::runtime_error("outbox message conflicts with bundle manifest");
            }
            OutboxEntry value;
            value.id = string_value(manifest, std::string(prefix) + "Id");
            value.message_type = prefix == "assessment"
                ? "BRAKE_HEALTH_ASSESSMENT" : "BRAKE_HEALTH_EVENT";
            value.canonical_json = encoded;
            value.content_sha256 = string_value(
                manifest, std::string(prefix) + "ContentSha256");
            value.idempotency_key_sha256 = string_value(
                manifest, std::string(prefix) + "IdempotencyKeySha256");
            value.quarantined = quarantined;
            bytes += encoded.size();
            result.push_back(std::move(value));
        };
        append("assessment", bundle / "assessment.json");
        if (optional_string_value(manifest, "eventId")) {
            append("event", bundle / "event.json");
        } else if (std::filesystem::exists(bundle / "event.json")) {
            throw std::runtime_error("unexpected event in assessment-only bundle");
        }
    }
    if (result.size() > kMaximumMessages || bytes > kMaximumOutboxBytes) {
        throw std::runtime_error("persisted outbox exceeds its accepted bounds");
    }
    std::sort(result.begin(), result.end(), [](const OutboxEntry& left, const OutboxEntry& right) {
        return left.id < right.id;
    });
    return result;
}

ModelState StateStore::state() const {
    return parse_state_json(read_bounded(state_root_ / "state.json"));
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
        const auto duplicate = std::find(
            before.recent_source_event_ids.begin(),
            before.recent_source_event_ids.end(),
            episode.source_event_id);
        if (duplicate != before.recent_source_event_ids.end()) {
            const std::string id = assessment_id(metadata, episode.source_event_id);
            if (before.last_applied_source_event_id == episode.source_event_id &&
                before.last_assessment_id != id) {
                ready_ = false;
                return {ProcessStatus::NotReadyState, std::nullopt, std::nullopt, false};
            }
            return {ProcessStatus::Duplicate, std::nullopt, id, false};
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

        const std::vector<OutboxEntry> current = inventory();
        std::size_t current_bytes = 0U;
        for (const OutboxEntry& value : current) {
            current_bytes += value.canonical_json.size();
        }
        const bool admit = current.size() + messages.count() <= kMaximumMessages &&
                           current_bytes + messages.encoded_bytes() <= kMaximumOutboxBytes;
        try {
            persist_transaction(before, after, messages, admit);
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
