// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "brake_health/runtime/runtime.hpp"
#include "brake_health/v2/state_store.hpp"

namespace brake_health::runtime {
// The model/runtime owner serializes these calls with StateStore::process.
// HTTP is executed independently, outside that owner lock. These functions
// do not start a sender or infer which functional profile is running.
std::optional<v2::OutboxEntry> next_derived_message(v2::StateStore& store);
bool accept_derived_message(v2::StateStore& store, const v2::OutboxEntry& pending,
                            const HttpResponse& response);
}  // namespace brake_health::runtime
