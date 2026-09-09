// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0
#include "brake_health/runtime/derived_delivery.hpp"
#include <algorithm>

namespace brake_health::runtime {
std::optional<v2::OutboxEntry> next_derived_message(v2::StateStore& store) {
    if (!store.ready()) return std::nullopt;
    const auto entries = store.inventory();
    const auto entry = std::find_if(entries.begin(), entries.end(),
        [](const v2::OutboxEntry& value) { return !value.quarantined; });
    if (entry == entries.end()) return std::nullopt;
    return *entry;
}
bool accept_derived_message(v2::StateStore& store, const v2::OutboxEntry& pending,
                            const HttpResponse& response) {
    if (!store.ready()) return false;
    const auto entries = store.inventory();
    const auto entry = std::find_if(entries.begin(), entries.end(),
        [&](const v2::OutboxEntry& value) { return value.id == pending.id; });
    if (entry == entries.end() || entry->quarantined) return false;
    if (entry->canonical_json != pending.canonical_json || entry->idempotency_key_sha256 != pending.idempotency_key_sha256 ||
        entry->content_sha256 != pending.content_sha256 || entry->message_sha256 != pending.message_sha256) {
        store.quarantine_delivery(pending.id);
        return false;
    }
    if (!matches_ack(pending.canonical_json, response)) {
        if (response.status != 0 && !retryable_http(response.status)) store.quarantine_delivery(pending.id);
        return false;
    }
    // Never pass a merely claimed receipt digest to the state store. The exact
    // closed ACK has been matched against the retained logical message first.
    return store.acknowledge(pending.id, pending.idempotency_key_sha256, pending.content_sha256);
}
}  // namespace brake_health::runtime
