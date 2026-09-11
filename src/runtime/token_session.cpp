// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0
#include "brake_health/runtime/token_session.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace brake_health::runtime {
namespace {
struct Fd {
    int value{-1};
    ~Fd() { if (value >= 0) ::close(value); }
    int release() { const int result = value; value = -1; return result; }
};
[[noreturn]] void invalid() { throw std::runtime_error("TOKEN_DIRECTORY_INVALID"); }
int open_directory(const std::filesystem::path& path) {
    if (!path.is_absolute() || path != path.lexically_normal()) invalid();
    Fd current{::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    if (current.value < 0) invalid();
    for (const auto& component : path.relative_path()) {
        if (component.empty() || component == "." || component == "..") invalid();
        const int next = ::openat(current.value, component.c_str(),
                                 O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (next < 0) invalid();
        ::close(current.value);
        current.value = next;
    }
    return current.release();
}
bool session_name(const std::string& name) {
    return name.size() == 14 && name.compare(0, 8, "session-") == 0 &&
        std::all_of(name.begin() + 8, name.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9');
        });
}
}
int open_private_token_directory(const std::filesystem::path& directory) {
    Fd fd{open_directory(directory)};
    struct stat st{};
    if (::fstat(fd.value, &st) != 0 || st.st_uid != ::geteuid() ||
        st.st_gid != ::getegid() || (st.st_mode & 07777) != 0700) invalid();
    return fd.release();
}
TokenSession::TokenSession(const std::filesystem::path& root) {
    Fd root_fd{open_directory(root)};
    struct stat st{};
    // Native tmpfs is root-owned. An explicit owned test root permits host tests.
    if (::fstat(root_fd.value, &st) != 0 || (st.st_mode & 07777) != 01777 ||
        st.st_uid != (root == token_root ? 0 : ::geteuid())) invalid();
    if (::flock(root_fd.value, LOCK_EX) != 0) invalid();
    struct Unlock { int fd; ~Unlock() { ::flock(fd, LOCK_UN); } } unlock{root_fd.value};
    Fd scan_fd{::dup(root_fd.value)};
    if (scan_fd.value < 0) invalid();
    DIR* directory = ::fdopendir(scan_fd.value);
    if (!directory) invalid();
    (void)scan_fd.release(); // fdopendir owns it after success.
    std::size_t sessions = 0;
    errno = 0;
    while (const auto* entry = ::readdir(directory)) {
        if (std::string(entry->d_name).compare(0, 8, "session-") == 0) ++sessions;
    }
    const int scan_error = errno;
    ::closedir(directory);
    if (scan_error != 0) invalid();
    // Fail closed; never guess whether a peer session is active or delete it.
    if (sessions >= 8) throw std::runtime_error("TOKEN_SESSION_CAPACITY_EXCEEDED");
    const auto pattern = (root / "session-XXXXXX").string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    if (!::mkdtemp(buffer.data())) invalid();
    token_file_ = std::filesystem::path(buffer.data()) / "token.jwt";
    session_name_ = token_file_.parent_path().filename().string();
    try {
        Fd session_fd{open_private_token_directory(token_file_.parent_path())};
        if (!session_name(session_name_)) invalid();
        session_fd_ = session_fd.release();
        root_fd_ = root_fd.release();
    } catch (...) {
        ::unlinkat(root_fd.value, session_name_.c_str(), AT_REMOVEDIR);
        throw;
    }
}
TokenSession::~TokenSession() {
    if (session_fd_ >= 0) {
        ::unlinkat(session_fd_, "token.jwt", 0);
        ::close(session_fd_);
    }
    if (root_fd_ >= 0) {
        ::unlinkat(root_fd_, session_name_.c_str(), AT_REMOVEDIR);
        ::close(root_fd_);
    }
}
void TokenSession::remove_token() const {
    if (::unlinkat(session_fd_, "token.jwt", 0) != 0 && errno != ENOENT)
        throw std::runtime_error("TOKEN_REMOVE_FAILED");
}
std::filesystem::path token_file_from_environment() {
    const char* value = std::getenv("KUKSA_TOKEN_FILE");
    if (!value) throw std::runtime_error("CREDENTIAL_ENVIRONMENT_INVALID");
    const std::filesystem::path path(value);
    if (path.filename() != "token.jwt" || path.parent_path().parent_path() != token_root ||
        !session_name(path.parent_path().filename().string()))
        throw std::runtime_error("CREDENTIAL_ENVIRONMENT_INVALID");
    Fd directory{open_private_token_directory(path.parent_path())};
    return path;
}
}  // namespace brake_health::runtime
