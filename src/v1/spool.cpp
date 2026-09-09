// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0

#include "brake_health/v1/spool.hpp"

#include "brake_health/v1/sha256.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>

namespace brake_health::v1 {
namespace {

constexpr std::size_t maximum_windows = 8U;
constexpr std::size_t maximum_encoded_bytes = 4U * 1024U * 1024U;
constexpr std::size_t maximum_message_bytes = 65536U;

bool valid_uuid4(const std::string& value) {
    if (value.size() != 36U || value[8] != '-' || value[13] != '-' ||
        value[18] != '-' || value[23] != '-' || value[14] != '4' ||
        std::string("89ab").find(value[19]) == std::string::npos) {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 8U || index == 13U || index == 18U || index == 23U) {
            continue;
        }
        const char character = value[index];
        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return true;
}

std::runtime_error posix_error(const std::string& action) {
    return std::runtime_error(action + ": " + std::strerror(errno));
}

SpoolState parse_state(const std::string& value) {
    if (value == "CAPTURING") {
        return SpoolState::Capturing;
    }
    if (value == "READY_TO_SEND") {
        return SpoolState::ReadyToSend;
    }
    if (value == "WAITING_ACK") {
        return SpoolState::WaitingAck;
    }
    if (value == "ACKNOWLEDGED") {
        return SpoolState::Acknowledged;
    }
    if (value == "QUARANTINED") {
        return SpoolState::Quarantined;
    }
    throw std::runtime_error("invalid spool state");
}

bool chunk_filename(const std::string& name) {
    return name.size() == 14U && name.compare(0, 6, "chunk-") == 0 &&
           name.compare(9, 5, ".json") == 0 &&
           std::all_of(name.begin() + 6, name.begin() + 9, [](char character) {
               return character >= '0' && character <= '9';
           });
}

}  // namespace

EventSpool::EventSpool(std::filesystem::path root, FaultInjector fault_injector)
    : root_(std::move(root)), fault_injector_(std::move(fault_injector)) {
    if (root_.empty()) {
        throw std::invalid_argument("spool root is required");
    }
    std::filesystem::create_directories(root_);
    if (::chmod(root_.c_str(), 0700) != 0) {
        throw posix_error("chmod spool root");
    }
}

bool EventSpool::should_fail(WriteStage stage, const std::filesystem::path& path) const {
    return fault_injector_ && fault_injector_(stage, path);
}

void EventSpool::sync_directory(const std::filesystem::path& directory) const {
    if (should_fail(WriteStage::DirectorySync, directory)) {
        throw std::runtime_error("injected directory fsync failure");
    }
    const int descriptor = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0) {
        throw posix_error("open directory for fsync");
    }
    if (::fsync(descriptor) != 0) {
        const int saved_errno = errno;
        ::close(descriptor);
        errno = saved_errno;
        throw posix_error("fsync directory");
    }
    if (::close(descriptor) != 0) {
        throw posix_error("close directory");
    }
}

void EventSpool::atomic_write(
    const std::filesystem::path& target, std::string_view bytes) const {
    const std::filesystem::path directory = target.parent_path();
    const std::filesystem::path temporary =
        directory / ("." + target.filename().string() + ".tmp-" +
                     std::to_string(::getpid()) + "-" +
                     std::to_string(temporary_counter_++));
    if (should_fail(WriteStage::TemporaryOpen, target)) {
        throw std::runtime_error("injected temporary open failure");
    }
    int descriptor = ::open(
        temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor < 0) {
        throw posix_error("open temporary spool file");
    }
    bool renamed = false;
    try {
        if (::fchmod(descriptor, 0600) != 0) {
            throw posix_error("chmod spool file");
        }
        if (should_fail(WriteStage::TemporaryWrite, target)) {
            throw std::runtime_error("injected temporary write failure");
        }
        std::size_t written = 0;
        while (written < bytes.size()) {
            const ssize_t count = ::write(
                descriptor, bytes.data() + written, bytes.size() - written);
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw posix_error("write temporary spool file");
            }
            written += static_cast<std::size_t>(count);
        }
        if (should_fail(WriteStage::FileSync, target)) {
            throw std::runtime_error("injected file fsync failure");
        }
        if (::fsync(descriptor) != 0) {
            throw posix_error("fsync spool file");
        }
        if (::close(descriptor) != 0) {
            throw posix_error("close spool file");
        }
        descriptor = -1;
        if (should_fail(WriteStage::Rename, target)) {
            throw std::runtime_error("injected rename failure");
        }
        if (::rename(temporary.c_str(), target.c_str()) != 0) {
            throw posix_error("rename spool file");
        }
        renamed = true;
        sync_directory(directory);
    } catch (...) {
        const int saved_errno = errno;
        if (descriptor >= 0) {
            ::close(descriptor);
        }
        if (!renamed) {
            ::unlink(temporary.c_str());
        }
        errno = saved_errno;
        throw;
    }
}

void EventSpool::write_state(
    const std::filesystem::path& directory, SpoolState state) const {
    atomic_write(directory / "manifest", spool_state_name(state));
}

std::filesystem::path EventSpool::event_directory(const std::string& event_id) const {
    if (!valid_uuid4(event_id)) {
        throw std::invalid_argument("event ID must be a lowercase UUIDv4");
    }
    return root_ / event_id;
}

AdmissionResult EventSpool::store_completed(
    const std::string& event_id, const MessageSet& messages) {
    return store(event_id, messages, false);
}

AdmissionResult EventSpool::store_capturing_for_recovery(
    const std::string& event_id, const MessageSet& aborted_restart_messages) {
    return store(event_id, aborted_restart_messages, true);
}

AdmissionResult EventSpool::store(
    const std::string& event_id, const MessageSet& messages, bool capturing) {
    const std::filesystem::path directory = event_directory(event_id);
    if (std::filesystem::exists(directory)) {
        throw std::invalid_argument("event already exists in spool");
    }
    if (messages.chunks.empty() || messages.chunks.size() > 15U ||
        messages.completion.canonical_json.empty()) {
        throw std::invalid_argument("message set is incomplete");
    }
    for (const CanonicalMessage& chunk : messages.chunks) {
        enforce_message_size(chunk.canonical_json);
    }
    enforce_message_size(messages.completion.canonical_json);

    const std::vector<SpoolEntry> current = inventory();
    std::size_t current_bytes = 0;
    for (const SpoolEntry& entry : current) {
        current_bytes += entry.encoded_bytes;
    }
    if (current.size() >= maximum_windows ||
        messages.encoded_bytes() > maximum_encoded_bytes -
            std::min(current_bytes, maximum_encoded_bytes)) {
        ++dropped_queue_full_;
        return AdmissionResult::WindowDroppedQueueFull;
    }

    if (!std::filesystem::create_directory(directory)) {
        throw std::runtime_error("failed to create event directory");
    }
    if (::chmod(directory.c_str(), 0700) != 0) {
        throw posix_error("chmod event directory");
    }
    sync_directory(root_);
    write_state(directory, SpoolState::Capturing);
    for (const CanonicalMessage& chunk : messages.chunks) {
        if (!chunk_filename(chunk.filename)) {
            throw std::invalid_argument("chunk filename is outside the closed layout");
        }
        atomic_write(directory / chunk.filename, chunk.canonical_json);
        atomic_write(
            directory / (chunk.filename + ".sha256"), sha256_hex(chunk.canonical_json));
    }
    const std::string completion_name =
        capturing ? "restart-completion.json" : "completion.json";
    atomic_write(directory / completion_name, messages.completion.canonical_json);
    atomic_write(
        directory / (completion_name + ".sha256"),
        sha256_hex(messages.completion.canonical_json));
    if (!capturing) {
        write_state(directory, SpoolState::ReadyToSend);
    }
    return AdmissionResult::Stored;
}

AdmissionResult EventSpool::checkpoint_capturing(
    const std::string& event_id, const MessageSet& messages) {
    const std::filesystem::path directory = event_directory(event_id);
    const SpoolEntry existing = inspect_event(directory);
    if (existing.state != SpoolState::Capturing || messages.chunks.empty() ||
        messages.chunks.size() > 15U) {
        throw std::logic_error("only a capturing event may be checkpointed");
    }
    std::size_t other_bytes = 0;
    for (const SpoolEntry& entry : inventory()) {
        if (entry.event_id != event_id) {
            other_bytes += entry.encoded_bytes;
        }
    }
    if (messages.encoded_bytes() >
        maximum_encoded_bytes - std::min(other_bytes, maximum_encoded_bytes)) {
        ++dropped_queue_full_;
        return AdmissionResult::WindowDroppedQueueFull;
    }
    for (const CanonicalMessage& chunk : messages.chunks) {
        if (!chunk_filename(chunk.filename)) {
            throw std::invalid_argument("chunk filename is outside the closed layout");
        }
        enforce_message_size(chunk.canonical_json);
        atomic_write(directory / chunk.filename, chunk.canonical_json);
        atomic_write(
            directory / (chunk.filename + ".sha256"), sha256_hex(chunk.canonical_json));
    }
    enforce_message_size(messages.completion.canonical_json);
    atomic_write(
        directory / "restart-completion.json", messages.completion.canonical_json);
    atomic_write(
        directory / "restart-completion.json.sha256",
        sha256_hex(messages.completion.canonical_json));
    return AdmissionResult::Stored;
}

AdmissionResult EventSpool::complete_capturing(const std::string& event_id, const MessageSet& messages) {
    const auto result = checkpoint_capturing(event_id, messages);
    if (result != AdmissionResult::Stored) return result;
    const auto directory = event_directory(event_id);
    if (::rename((directory / "restart-completion.json").c_str(), (directory / "completion.json").c_str()) != 0 ||
        ::rename((directory / "restart-completion.json.sha256").c_str(), (directory / "completion.json.sha256").c_str()) != 0) {
        throw posix_error("publish captured completion");
    }
    sync_directory(directory);
    write_state(directory, SpoolState::ReadyToSend);
    return result;
}

void EventSpool::quarantine(const std::string& event_id) {
    const auto directory = event_directory(event_id);
    static_cast<void>(inspect_event(directory));
    write_state(directory, SpoolState::Quarantined);
}

bool EventSpool::valid_message_file(const std::filesystem::path& path) const {
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error || size == 0U || size > maximum_message_bytes) {
        return false;
    }
    std::ifstream stream(path, std::ios::binary);
    const std::string bytes{
        std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    if (!stream.is_open() || stream.bad() || bytes.empty() || bytes.front() != '{' ||
        bytes.back() != '}') {
        return false;
    }
    std::ifstream integrity_stream(path.string() + ".sha256", std::ios::binary);
    const std::string integrity{
        std::istreambuf_iterator<char>(integrity_stream),
        std::istreambuf_iterator<char>()};
    if (!integrity_stream.is_open() || integrity_stream.bad() || integrity.size() != 64U ||
        sha256_hex(bytes) != integrity) {
        return false;
    }

    static constexpr std::string_view prefix = "{\"content\":";
    if (bytes.compare(0, prefix.size(), prefix) != 0 || bytes[prefix.size()] != '{') {
        return false;
    }
    std::size_t index = prefix.size();
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (; index < bytes.size(); ++index) {
        const char character = bytes[index];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (character == '\\') {
                escaped = true;
            } else if (character == '"') {
                in_string = false;
            }
        } else if (character == '"') {
            in_string = true;
        } else if (character == '{') {
            ++depth;
        } else if (character == '}' && --depth == 0) {
            ++index;
            break;
        }
    }
    static constexpr std::string_view marker = ",\"contentSha256\":\"";
    if (depth != 0 || index + marker.size() + 65U > bytes.size() ||
        bytes.compare(index, marker.size(), marker) != 0) {
        return false;
    }
    const std::string expected = bytes.substr(index + marker.size(), 64U);
    if (bytes[index + marker.size() + 64U] != '"' ||
        sha256_hex(std::string_view(bytes).substr(prefix.size(), index - prefix.size())) !=
            expected) {
        return false;
    }
    return std::all_of(expected.begin(), expected.end(), [](char character) {
        return (character >= '0' && character <= '9') ||
               (character >= 'a' && character <= 'f');
    });
}

SpoolEntry EventSpool::inspect_event(const std::filesystem::path& directory) const {
    std::ifstream state_stream(directory / "manifest", std::ios::binary);
    const std::string state_value{
        std::istreambuf_iterator<char>(state_stream), std::istreambuf_iterator<char>()};
    if (!state_stream.is_open() || state_stream.bad()) {
        throw std::runtime_error("cannot read spool state");
    }
    SpoolEntry result;
    result.event_id = directory.filename().string();
    result.state = parse_state(state_value);
    for (const std::filesystem::directory_entry& item :
         std::filesystem::directory_iterator(directory)) {
        if (!item.is_regular_file() || item.is_symlink()) {
            continue;
        }
        const std::string name = item.path().filename().string();
        if (chunk_filename(name)) {
            ++result.chunk_count;
            result.encoded_bytes += static_cast<std::size_t>(item.file_size());
        } else if (name == "completion.json" || name == "restart-completion.json") {
            result.encoded_bytes += static_cast<std::size_t>(item.file_size());
            if (name == "completion.json") {
                result.completion_present = true;
            }
        }
    }
    return result;
}

std::vector<SpoolEntry> EventSpool::inventory() const {
    std::vector<SpoolEntry> result;
    for (const std::filesystem::directory_entry& item :
         std::filesystem::directory_iterator(root_)) {
        if (item.is_directory() && !item.is_symlink() &&
            valid_uuid4(item.path().filename().string())) {
            result.push_back(inspect_event(item.path()));
        }
    }
    std::sort(result.begin(), result.end(), [](const SpoolEntry& left, const SpoolEntry& right) {
        return left.event_id < right.event_id;
    });
    return result;
}

std::vector<SpoolEntry> EventSpool::recover() {
    for (const std::filesystem::directory_entry& item :
         std::filesystem::directory_iterator(root_)) {
        if (!item.is_directory() || item.is_symlink() ||
            !valid_uuid4(item.path().filename().string())) {
            continue;
        }
        const std::filesystem::path directory = item.path();
        try {
            const SpoolEntry entry = inspect_event(directory);
            bool chunks_valid = entry.chunk_count > 0U;
            for (const std::filesystem::directory_entry& file :
                 std::filesystem::directory_iterator(directory)) {
                if (file.is_regular_file() &&
                    chunk_filename(file.path().filename().string()) &&
                    !valid_message_file(file.path())) {
                    chunks_valid = false;
                }
            }
            if (entry.state == SpoolState::Capturing) {
                const std::filesystem::path restart = directory / "restart-completion.json";
                if (!chunks_valid || !valid_message_file(restart)) {
                    write_state(directory, SpoolState::Quarantined);
                    continue;
                }
                if (::rename(restart.c_str(), (directory / "completion.json").c_str()) != 0) {
                    throw posix_error("publish restart completion");
                }
                const std::filesystem::path restart_integrity =
                    directory / "restart-completion.json.sha256";
                if (::rename(
                        restart_integrity.c_str(),
                        (directory / "completion.json.sha256").c_str()) != 0) {
                    throw posix_error("publish restart completion integrity");
                }
                sync_directory(directory);
                write_state(directory, SpoolState::ReadyToSend);
            } else if (entry.state != SpoolState::Quarantined &&
                       (!chunks_valid || !entry.completion_present ||
                        !valid_message_file(directory / "completion.json"))) {
                write_state(directory, SpoolState::Quarantined);
            }
        } catch (...) {
            write_state(directory, SpoolState::Quarantined);
        }
    }
    return inventory();
}

void EventSpool::mark_waiting_ack(const std::string& event_id) {
    const std::filesystem::path directory = event_directory(event_id);
    const SpoolEntry entry = inspect_event(directory);
    if (entry.state != SpoolState::ReadyToSend) {
        throw std::logic_error("only a ready event may wait for acknowledgement");
    }
    write_state(directory, SpoolState::WaitingAck);
}

void EventSpool::acknowledge_chunk(const std::string& event_id, std::size_t chunk_index) {
    if (chunk_index > 14U) {
        throw std::invalid_argument("chunk index exceeds contract bound");
    }
    const std::filesystem::path directory = event_directory(event_id);
    std::ostringstream name;
    name << "chunk-" << std::setw(3) << std::setfill('0') << chunk_index << ".json";
    if (!std::filesystem::is_regular_file(directory / name.str())) {
        throw std::invalid_argument("acknowledged chunk does not exist");
    }
    atomic_write(directory / (name.str() + ".ack"), "DURABLE_ACK");
}

void EventSpool::acknowledge_completion(const std::string& event_id) {
    const std::filesystem::path directory = event_directory(event_id);
    if (!std::filesystem::is_regular_file(directory / "completion.json")) {
        throw std::invalid_argument("completion does not exist");
    }
    atomic_write(directory / "completion.json.ack", "DURABLE_ACK");
}

bool EventSpool::delete_if_fully_acknowledged(const std::string& event_id) {
    const std::filesystem::path directory = event_directory(event_id);
    const SpoolEntry entry = inspect_event(directory);
    if (entry.state != SpoolState::WaitingAck) {
        return false;
    }
    for (const std::filesystem::directory_entry& file :
         std::filesystem::directory_iterator(directory)) {
        const std::string name = file.path().filename().string();
        if (file.is_regular_file() && chunk_filename(name) &&
            !std::filesystem::is_regular_file(directory / (name + ".ack"))) {
            return false;
        }
    }
    if (!std::filesystem::is_regular_file(directory / "completion.json.ack")) {
        return false;
    }
    write_state(directory, SpoolState::Acknowledged);
    std::filesystem::remove_all(directory);
    sync_directory(root_);
    return true;
}

std::size_t EventSpool::dropped_queue_full() const {
    return dropped_queue_full_;
}

const char* spool_state_name(SpoolState state) {
    switch (state) {
        case SpoolState::Capturing:
            return "CAPTURING";
        case SpoolState::ReadyToSend:
            return "READY_TO_SEND";
        case SpoolState::WaitingAck:
            return "WAITING_ACK";
        case SpoolState::Acknowledged:
            return "ACKNOWLEDGED";
        case SpoolState::Quarantined:
            return "QUARANTINED";
    }
    throw std::invalid_argument("unknown spool state");
}

}  // namespace brake_health::v1
