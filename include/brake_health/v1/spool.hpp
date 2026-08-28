// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "brake_health/v1/messages.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace brake_health::v1 {

enum class SpoolState {
    Capturing,
    ReadyToSend,
    WaitingAck,
    Acknowledged,
    Quarantined,
};

enum class WriteStage {
    TemporaryOpen,
    TemporaryWrite,
    FileSync,
    Rename,
    DirectorySync,
};

using FaultInjector = std::function<bool(WriteStage, const std::filesystem::path&)>;

enum class AdmissionResult { Stored, WindowDroppedQueueFull };

struct SpoolEntry {
    std::string event_id;
    SpoolState state{SpoolState::Capturing};
    std::size_t encoded_bytes{};
    std::size_t chunk_count{};
    bool completion_present{};
};

class EventSpool {
public:
    explicit EventSpool(std::filesystem::path root, FaultInjector fault_injector = {});

    AdmissionResult store_completed(const std::string& event_id, const MessageSet& messages);
    AdmissionResult store_capturing_for_recovery(
        const std::string& event_id,
        const MessageSet& aborted_restart_messages);
    AdmissionResult checkpoint_capturing(
        const std::string& event_id,
        const MessageSet& aborted_restart_messages);
    std::vector<SpoolEntry> recover();
    std::vector<SpoolEntry> inventory() const;
    void mark_waiting_ack(const std::string& event_id);
    void acknowledge_chunk(const std::string& event_id, std::size_t chunk_index);
    void acknowledge_completion(const std::string& event_id);
    bool delete_if_fully_acknowledged(const std::string& event_id);
    std::size_t dropped_queue_full() const;

private:
    AdmissionResult store(
        const std::string& event_id,
        const MessageSet& messages,
        bool capturing);
    void atomic_write(const std::filesystem::path& target, std::string_view bytes) const;
    void sync_directory(const std::filesystem::path& directory) const;
    void write_state(const std::filesystem::path& directory, SpoolState state) const;
    SpoolEntry inspect_event(const std::filesystem::path& directory) const;
    bool valid_message_file(const std::filesystem::path& path) const;
    bool should_fail(WriteStage stage, const std::filesystem::path& path) const;
    std::filesystem::path event_directory(const std::string& event_id) const;

    std::filesystem::path root_;
    FaultInjector fault_injector_;
    mutable std::size_t temporary_counter_{};
    std::size_t dropped_queue_full_{};
};

const char* spool_state_name(SpoolState state);

}  // namespace brake_health::v1
