// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace brake_health::v1 {

using Sha256Digest = std::array<std::uint8_t, 32>;

Sha256Digest sha256(std::string_view input);
std::string sha256_hex(std::string_view input);
std::string hex_encode(const Sha256Digest& digest);
Sha256Digest hex_decode_sha256(std::string_view value);

}  // namespace brake_health::v1
