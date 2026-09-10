// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace brake_health::runtime {
// Bounded strict JSON for protocol envelopes. Product canonical serialization
// remains owned by the existing v1/v2 domain libraries.
struct Json {
    using Object = std::map<std::string, Json>;
    using Array = std::vector<Json>;
    std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, Object, Array> value;
    const Object& object() const;
    const Json& at(const std::string& key) const;
    const std::string& string() const;
    std::int64_t integer() const;
    bool boolean() const;
};
Json parse_json(std::string_view text, std::size_t limit = 65536);
std::string quote_json(std::string_view text);
// Canonical structural JSON for private integer/string runtime ledgers.
// Floating point is deliberately unsupported; product model values have their
// own fixed-point canonical serializer.
std::string encode_json(const Json& value);
bool is_sha256(std::string_view text);
bool is_uuid(std::string_view text);
}  // namespace brake_health::runtime
