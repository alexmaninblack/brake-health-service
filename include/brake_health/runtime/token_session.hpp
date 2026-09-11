// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <filesystem>

namespace brake_health::runtime {
inline constexpr const char* token_root = "/run/aosedge/secrets/kuksa";

// Owned by the bootstrap, never copied or adopted on restart.
class TokenSession final {
public:
    explicit TokenSession(const std::filesystem::path& root = token_root);
    ~TokenSession();
    TokenSession(const TokenSession&) = delete;
    TokenSession& operator=(const TokenSession&) = delete;
    const std::filesystem::path& token_file() const { return token_file_; }
    void remove_token() const;
private:
    int root_fd_{-1};
    int session_fd_{-1};
    std::filesystem::path token_file_;
    std::string session_name_;
};

// Walk every parent without following symlinks; caller owns the returned fd.
int open_private_token_directory(const std::filesystem::path& directory);
std::filesystem::path token_file_from_environment();
}  // namespace brake_health::runtime
