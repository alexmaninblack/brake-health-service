// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "brake_health/v1/window.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace brake_health::v1 {

enum class UnitRole { Validation, Production };

struct MessageMetadata {
    std::string unit_system_uid;
    UnitRole unit_role{UnitRole::Validation};
    std::string service_version;
    std::string service_artifact_sha256;
    std::string vdp_contract_version;
    std::string vdp_contract_sha256;
};

struct CanonicalMessage {
    std::string filename;
    std::string canonical_json;
    std::string content_sha256;
};

struct MessageSet {
    std::vector<CanonicalMessage> chunks;
    CanonicalMessage completion;

    std::size_t encoded_bytes() const;
};

MessageSet build_messages(const MessageMetadata& metadata, const EventWindow& window);
// Seal PRE separately so its final short chunk is immutable at trigger time.
// Existing completed-message/golden-vector serialization remains available above.
MessageSet build_growing_messages(const MessageMetadata& metadata, const EventWindow& window);
std::string canonicalize_chunk_content(
    std::size_t chunk_index,
    std::size_t first_sample_index,
    const std::vector<RetainedSample>& samples);
std::string canonicalize_completion_content(
    const EventWindow& window,
    const std::vector<std::string>& chunk_content_sha256);
std::string window_sha256(const std::vector<std::string>& chunk_content_sha256);
void enforce_message_size(std::string_view message);

const char* unit_role_name(UnitRole role);

}  // namespace brake_health::v1
