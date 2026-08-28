// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0

#include "brake_health/v1/sha256.hpp"

#include <array>
#include <stdexcept>
#include <string>
#include <vector>

namespace brake_health::v1 {
namespace {

constexpr std::array<std::uint32_t, 64> round_constants = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U,
    0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U,
    0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
    0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU,
    0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

std::uint32_t rotate_right(std::uint32_t value, unsigned count) {
    return (value >> count) | (value << (32U - count));
}

int hex_value(char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    return -1;
}

}  // namespace

Sha256Digest sha256(std::string_view input) {
    std::vector<std::uint8_t> message(input.begin(), input.end());
    const std::uint64_t bit_length = static_cast<std::uint64_t>(message.size()) * 8U;
    message.push_back(0x80U);
    while (message.size() % 64U != 56U) {
        message.push_back(0U);
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
        message.push_back(static_cast<std::uint8_t>((bit_length >> shift) & 0xffU));
    }

    std::array<std::uint32_t, 8> state = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
    };

    for (std::size_t offset = 0; offset < message.size(); offset += 64U) {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0; index < 16U; ++index) {
            const std::size_t at = offset + index * 4U;
            words[index] = (static_cast<std::uint32_t>(message[at]) << 24U) |
                           (static_cast<std::uint32_t>(message[at + 1U]) << 16U) |
                           (static_cast<std::uint32_t>(message[at + 2U]) << 8U) |
                           static_cast<std::uint32_t>(message[at + 3U]);
        }
        for (std::size_t index = 16U; index < 64U; ++index) {
            const std::uint32_t s0 = rotate_right(words[index - 15U], 7U) ^
                                     rotate_right(words[index - 15U], 18U) ^
                                     (words[index - 15U] >> 3U);
            const std::uint32_t s1 = rotate_right(words[index - 2U], 17U) ^
                                     rotate_right(words[index - 2U], 19U) ^
                                     (words[index - 2U] >> 10U);
            words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
        }

        std::uint32_t a = state[0];
        std::uint32_t b = state[1];
        std::uint32_t c = state[2];
        std::uint32_t d = state[3];
        std::uint32_t e = state[4];
        std::uint32_t f = state[5];
        std::uint32_t g = state[6];
        std::uint32_t h = state[7];
        for (std::size_t index = 0; index < 64U; ++index) {
            const std::uint32_t s1 = rotate_right(e, 6U) ^ rotate_right(e, 11U) ^
                                     rotate_right(e, 25U);
            const std::uint32_t choice = (e & f) ^ (~e & g);
            const std::uint32_t temporary1 =
                h + s1 + choice + round_constants[index] + words[index];
            const std::uint32_t s0 = rotate_right(a, 2U) ^ rotate_right(a, 13U) ^
                                     rotate_right(a, 22U);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temporary2 = s0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temporary1;
            d = c;
            c = b;
            b = a;
            a = temporary1 + temporary2;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
    }

    Sha256Digest digest{};
    for (std::size_t index = 0; index < state.size(); ++index) {
        digest[index * 4U] = static_cast<std::uint8_t>((state[index] >> 24U) & 0xffU);
        digest[index * 4U + 1U] = static_cast<std::uint8_t>((state[index] >> 16U) & 0xffU);
        digest[index * 4U + 2U] = static_cast<std::uint8_t>((state[index] >> 8U) & 0xffU);
        digest[index * 4U + 3U] = static_cast<std::uint8_t>(state[index] & 0xffU);
    }
    return digest;
}

std::string hex_encode(const Sha256Digest& digest) {
    static constexpr char alphabet[] = "0123456789abcdef";
    std::string output;
    output.reserve(64U);
    for (std::uint8_t byte : digest) {
        output.push_back(alphabet[byte >> 4U]);
        output.push_back(alphabet[byte & 0x0fU]);
    }
    return output;
}

std::string sha256_hex(std::string_view input) {
    return hex_encode(sha256(input));
}

Sha256Digest hex_decode_sha256(std::string_view value) {
    if (value.size() != 64U) {
        throw std::invalid_argument("SHA-256 hex value must contain 64 characters");
    }
    Sha256Digest output{};
    for (std::size_t index = 0; index < output.size(); ++index) {
        const int high = hex_value(value[index * 2U]);
        const int low = hex_value(value[index * 2U + 1U]);
        if (high < 0 || low < 0) {
            throw std::invalid_argument("SHA-256 hex value must be lowercase hexadecimal");
        }
        output[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return output;
}

}  // namespace brake_health::v1
