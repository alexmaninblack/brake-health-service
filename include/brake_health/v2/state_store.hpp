// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "brake_health/v2/messages.hpp"

#include <filesystem>
#include <functional>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace brake_health::v2 {

enum class WriteStage {
    JournalFiles,
    Journal,
    State,
    IdentityLedger,
    BundleFiles,
    Bundle,
    CommitMarker,
    JournalRemoval,
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
    std::string message_sha256;
    std::string source_event_id;
    std::string assessment_id;
    bool quarantined{};
};

bool derived_outbox_admissible(
    std::size_t current_count,
    std::size_t current_bytes,
    std::size_t incoming_count,
    std::size_t incoming_bytes) noexcept;

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
        const SyntheticModel& model = {},
        std::size_t other_outbox_count = 0,
        std::size_t other_outbox_bytes = 0);
    std::vector<OutboxEntry> inventory() const;
    bool acknowledge(
        const std::string& id,
        const std::string& idempotency_key_sha256,
        const std::string& content_sha256);
    bool quarantine_delivery(const std::string& id);
    ModelState state() const;
    bool ready() const { return ready_; }
    // Explicit demo reset, not an assessment. Keeps identity, sequence and outbox.
    void reset_demo(const std::string& command_id);
    bool demo_reset_applied(const std::string& command_id) const;

private:
    struct IdentityBinding {
        std::string source_event_id;
        std::string assessment_id;
    };

    struct IdentityLedger {
        std::uint64_t generation{};
        std::string state_sha256;
        std::vector<IdentityBinding> entries;
    };

    void recover();
    void recover_demo_reset();
    void persist_transaction(
        const ModelState& before,
        const ModelState& after,
        const IdentityLedger& before_identities,
        const IdentityLedger& after_identities,
        const DerivedMessages& messages,
        bool admit);
    void atomic_write(const std::filesystem::path& target, std::string_view bytes) const;
    void sync_directory(const std::filesystem::path& directory) const;
    void fail_if_requested(WriteStage stage) const;
    std::vector<OutboxEntry> inventory_verified() const;
    IdentityLedger identity_ledger(const ModelState& state) const;
    static std::string identity_ledger_json(const IdentityLedger& ledger);
    static IdentityLedger parse_identity_ledger(std::string_view bytes);
    static void validate_identity_ledger(
        const IdentityLedger& ledger,
        const ModelState& state,
        std::string_view state_bytes);

    std::filesystem::path state_root_;
    std::filesystem::path outbox_root_;
    std::string producer_epoch_;
    FaultInjector fault_injector_;
    mutable std::uint64_t temporary_counter_{};
    mutable bool ready_{true};
};

}  // namespace brake_health::v2
