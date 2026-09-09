// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0
#include "brake_health/runtime/json.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <stdexcept>

namespace brake_health::runtime {
namespace {
[[noreturn]] void invalid() { throw std::invalid_argument("INVALID_JSON"); }
void utf8(std::string& out, unsigned code) {
    if (code < 0x80) out += static_cast<char>(code);
    else if (code < 0x800) {
        out += static_cast<char>(0xc0 | (code >> 6));
        out += static_cast<char>(0x80 | (code & 63));
    } else if (code < 0x10000) {
        out += static_cast<char>(0xe0 | (code >> 12));
        out += static_cast<char>(0x80 | ((code >> 6) & 63));
        out += static_cast<char>(0x80 | (code & 63));
    } else {
        out += static_cast<char>(0xf0 | (code >> 18));
        out += static_cast<char>(0x80 | ((code >> 12) & 63));
        out += static_cast<char>(0x80 | ((code >> 6) & 63));
        out += static_cast<char>(0x80 | (code & 63));
    }
}
void validate_utf8(std::string_view value) {
    for (std::size_t i = 0; i < value.size();) {
        unsigned c = static_cast<unsigned char>(value[i++]);
        if (c < 128) continue;
        unsigned n = c >= 0xc2 && c <= 0xdf ? 1 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : 9;
        if (n == 9 || i + n > value.size()) invalid();
        unsigned cp = c & ((1U << (6 - n)) - 1);
        for (unsigned j = 0; j < n; ++j) {
            unsigned part = static_cast<unsigned char>(value[i++]);
            if ((part & 0xc0) != 0x80) invalid();
            cp = (cp << 6) | (part & 63);
        }
        if (cp < (n == 1 ? 128U : n == 2 ? 2048U : 65536U) || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) invalid();
    }
}
class Parser {
public:
    explicit Parser(std::string_view input) : text(input) {}
    Json parse() { auto value = item(0); space(); if (pos != text.size()) invalid(); return value; }
private:
    std::string_view text;
    std::size_t pos{};
    void space() { while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\n' || text[pos] == '\t' || text[pos] == '\r')) ++pos; }
    bool take(char c) { space(); if (pos < text.size() && text[pos] == c) { ++pos; return true; } return false; }
    void require(char c) { if (!take(c)) invalid(); }
    unsigned hex4() {
        unsigned out = 0;
        for (unsigned i = 0; i < 4; ++i) {
            if (pos == text.size()) invalid();
            char c = text[pos++];
            unsigned h = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
            if (h == 16) invalid();
            out = out * 16 + h;
        }
        return out;
    }
    std::string string() {
        require('"'); std::string out;
        while (pos < text.size()) {
            auto c = static_cast<unsigned char>(text[pos++]);
            if (c == '"') { validate_utf8(out); return out; }
            if (c < 32) invalid();
            if (c != '\\') { out += static_cast<char>(c); continue; }
            if (pos == text.size()) invalid();
            switch (text[pos++]) {
                case '"': out += '"'; break; case '\\': out += '\\'; break;
                case '/': out += '/'; break; case 'b': out += '\b'; break;
                case 'f': out += '\f'; break; case 'n': out += '\n'; break;
                case 'r': out += '\r'; break; case 't': out += '\t'; break;
                case 'u': {
                    unsigned code = hex4();
                    if (code >= 0xd800 && code <= 0xdbff) {
                        if (pos + 2 > text.size() || text.substr(pos, 2) != "\\u") invalid();
                        pos += 2; unsigned low = hex4();
                        if (low < 0xdc00 || low > 0xdfff) invalid();
                        code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00;
                    } else if (code >= 0xdc00 && code <= 0xdfff) invalid();
                    utf8(out, code); break;
                }
                default: invalid();
            }
        }
        invalid();
    }
    Json item(unsigned depth) {
        if (depth > 24) invalid();
        space(); if (pos == text.size()) invalid();
        if (text[pos] == '"') return Json{string()};
        if (take('{')) {
            Json::Object obj;
            if (take('}')) return Json{obj};
            do { auto key = string(); require(':'); if (!obj.emplace(std::move(key), item(depth + 1)).second) invalid(); } while (take(','));
            require('}'); return Json{std::move(obj)};
        }
        if (take('[')) {
            Json::Array arr;
            if (take(']')) return Json{arr};
            do { arr.push_back(item(depth + 1)); } while (take(','));
            require(']'); return Json{std::move(arr)};
        }
        if (text.substr(pos, 4) == "true") { pos += 4; return Json{true}; }
        if (text.substr(pos, 5) == "false") { pos += 5; return Json{false}; }
        if (text.substr(pos, 4) == "null") { pos += 4; return Json{nullptr}; }
        const auto start = pos;
        if (text[pos] == '-') ++pos;
        if (pos == text.size()) invalid();
        if (text[pos] == '0') ++pos;
        else { if (text[pos] < '1' || text[pos] > '9') invalid(); while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos; }
        bool floating = false;
        if (pos < text.size() && text[pos] == '.') {
            floating = true; auto digits = ++pos;
            while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos;
            if (pos == digits) invalid();
        }
        if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
            floating = true; ++pos;
            if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) ++pos;
            auto digits = pos;
            while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos;
            if (pos == digits) invalid();
        }
        if (!floating) {
            std::int64_t v{}; auto r = std::from_chars(text.data() + start, text.data() + pos, v);
            if (r.ec != std::errc{}) invalid();
            return Json{v};
        }
        double v{}; auto r = std::from_chars(text.data() + start, text.data() + pos, v);
        if (r.ec != std::errc{} || !std::isfinite(v)) invalid();
        return Json{v};
    }
};
}  // namespace
const Json::Object& Json::object() const { return std::get<Object>(value); }
const Json& Json::at(const std::string& key) const { return object().at(key); }
const std::string& Json::string() const { return std::get<std::string>(value); }
std::int64_t Json::integer() const { return std::get<std::int64_t>(value); }
bool Json::boolean() const { return std::get<bool>(value); }
Json parse_json(std::string_view text, std::size_t limit) { if (text.size() > limit) invalid(); return Parser(text).parse(); }
std::string quote_json(std::string_view text) {
    validate_utf8(text); std::string out = "\""; constexpr char h[] = "0123456789abcdef";
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c == '\b') out += "\\b";
        else if (c == '\t') out += "\\t";
        else if (c == '\n') out += "\\n";
        else if (c == '\f') out += "\\f";
        else if (c == '\r') out += "\\r";
        else if (c < 32) { out += "\\u00"; out += h[c >> 4]; out += h[c & 15]; }
        else out += static_cast<char>(c);
    }
    return out + '"';
}
bool is_sha256(std::string_view text) { return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }); }
bool is_uuid(std::string_view text) {
    if (text.size() != 36) return false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (text[i] != '-') return false; }
        else if (!(text[i] >= '0' && text[i] <= '9') && !(text[i] >= 'a' && text[i] <= 'f') && !(text[i] >= 'A' && text[i] <= 'F')) return false;
    }
    return true;
}
}  // namespace brake_health::runtime
