// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "brake_health/v2/messages.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace brake_health::v2 {

enum class WriteStage {
    Journal,
    State,
    Bundle,
    CommitMarker,
};

using FaultInjector = std::function<bool(WriteStage)>;

enum class ProcessStatus {
    Produced,
    Duplicate,
    SkippedInputQuality,
    DerivedOutboxFull,
    NotReadyState,
};

struct ProcessResult {
    ProcessStatus status{ProcessStatus::NotReadyState};
    std::optional<SkipReason> skip_reason;
    std::optional<std::string> assessment_id;
    bool event_created{};
};

struct OutboxEntry {
    std::string id;
    std::string message_type;
    std::string canonical_json;
    std::string content_sha256;
    std::string idempotency_key_sha256;
    bool quarantined{};
};

class StateStore {
public:
    StateStore(
        std::filesystem::path state_root,
        std::filesystem::path outbox_root,
        std::string producer_epoch,
        FaultInjector fault_injector = {});

    ProcessResult process(
        const CompletedEpisode& episode,
        const DeploymentMetadata& metadata,
        const SyntheticModel& model = {});
    std::vector<OutboxEntry> inventory() const;
    bool acknowledge(
        const std::string& id,
        const std::string& idempotency_key_sha256,
        const std::string& content_sha256);
    ModelState state() const;
    bool ready() const { return ready_; }

private:
    void recover();
    void persist_transaction(
        const ModelState& before,
        const ModelState& after,
        const DerivedMessages& messages,
        bool admit);
    void atomic_write(const std::filesystem::path& target, std::string_view bytes) const;
    void sync_directory(const std::filesystem::path& directory) const;
    void fail_if_requested(WriteStage stage) const;

    std::filesystem::path state_root_;
    std::filesystem::path outbox_root_;
    std::string producer_epoch_;
    FaultInjector fault_injector_;
    mutable std::uint64_t temporary_counter_{};
    bool ready_{true};
};

}  // namespace brake_health::v2
