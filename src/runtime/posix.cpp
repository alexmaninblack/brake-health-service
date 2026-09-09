// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0
#include "brake_health/runtime/runtime.hpp"

#include <arpa/inet.h>
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <map>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace brake_health::runtime {
namespace {
struct Fd { int value{-1}; ~Fd() { if (value >= 0) ::close(value); } };
[[noreturn]] void fail() { throw std::runtime_error("TRANSPORT_UNAVAILABLE"); }
using Clock = std::chrono::steady_clock;
void wait(int fd, short events, Clock::time_point deadline, const std::atomic<bool>& stop) {
    while (!stop.load() && Clock::now() < deadline) {
        pollfd p{fd, events, 0};
        int result = ::poll(&p, 1, 100);
        if (result < 0 && errno != EINTR) fail();
        if (result > 0 && (p.revents & (events | POLLHUP))) return;
        if (result > 0 && (p.revents & (POLLERR | POLLNVAL))) fail();
    }
    fail();
}
std::string exchange(int family, const sockaddr* address, socklen_t length, const std::string& request,
    std::size_t maximum, int seconds, const std::atomic<bool>& stop) {
    Fd socket{::socket(family, SOCK_STREAM, 0)};
    if (socket.value < 0 || ::fcntl(socket.value, F_SETFD, FD_CLOEXEC) != 0 || ::fcntl(socket.value, F_SETFL, O_NONBLOCK) != 0) fail();
#ifdef SO_NOSIGPIPE
    int one = 1; ::setsockopt(socket.value, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    const auto deadline = Clock::now() + std::chrono::seconds(seconds);
    if (::connect(socket.value, address, length) != 0 && errno != EINPROGRESS) fail();
    wait(socket.value, POLLOUT, deadline, stop);
    int error{}; socklen_t size = sizeof(error);
    if (::getsockopt(socket.value, SOL_SOCKET, SO_ERROR, &error, &size) != 0 || error != 0) fail();
    std::size_t offset = 0;
    while (offset < request.size()) {
        wait(socket.value, POLLOUT, deadline, stop);
        const auto count = ::send(socket.value, request.data() + offset, request.size() - offset,
#ifdef MSG_NOSIGNAL
                                  MSG_NOSIGNAL
#else
                                  0
#endif
        );
        if (count < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (count <= 0) fail();
        offset += static_cast<std::size_t>(count);
    }
    std::string result;
    for (;;) {
        wait(socket.value, POLLIN, deadline, stop);
        char buffer[4096]; const auto count = ::recv(socket.value, buffer, sizeof(buffer), 0);
        if (count < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (count < 0) fail();
        if (count == 0) break;
        if (result.size() + static_cast<std::size_t>(count) > maximum) fail();
        result.append(buffer, static_cast<std::size_t>(count));
    }
    return result;
}
std::size_t number(std::string_view text, int base = 10) {
    std::size_t n{}; auto r = std::from_chars(text.data(), text.data() + text.size(), n, base);
    if (text.empty() || r.ec != std::errc{} || r.ptr != text.data() + text.size()) fail();
    return n;
}
}  // namespace
std::string read_file(const std::filesystem::path& path, std::size_t limit) {
    Fd file{::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
    struct stat info{};
    if (file.value < 0 || ::fstat(file.value, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size <= 0 || static_cast<std::uint64_t>(info.st_size) > limit) throw std::runtime_error("INPUT_FILE_UNAVAILABLE");
    std::string result;
    char data[4096];
    for (;;) {
        auto n = ::read(file.value, data, sizeof(data));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) throw std::runtime_error("INPUT_FILE_UNAVAILABLE");
        if (n == 0) break;
        result.append(data, static_cast<std::size_t>(n));
        if (result.size() > limit) throw std::runtime_error("INPUT_FILE_UNAVAILABLE");
    }
    return result;
}
void atomic_private_file(const std::filesystem::path& path, const std::string& bytes) {
    struct stat info{};
    if (::lstat(path.parent_path().c_str(), &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != ::geteuid() || (info.st_mode & 0777) != 0700) throw std::runtime_error("TOKEN_DIRECTORY_INVALID");
    const auto temporary = path.string() + ".next-" + std::to_string(::getpid());
    Fd file{::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600)};
    if (file.value < 0) throw std::runtime_error("TOKEN_WRITE_FAILED");
    try {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            auto n = ::write(file.value, bytes.data() + offset, bytes.size() - offset);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) throw std::runtime_error("TOKEN_WRITE_FAILED");
            offset += static_cast<std::size_t>(n);
        }
        if (::fsync(file.value) != 0 || ::rename(temporary.c_str(), path.c_str()) != 0) throw std::runtime_error("TOKEN_WRITE_FAILED");
    } catch (...) { ::unlink(temporary.c_str()); throw; }
}
std::string random_uuid() {
    Fd random{::open("/dev/urandom", O_RDONLY | O_CLOEXEC)};
    std::array<unsigned char, 16> bytes{}; std::size_t offset = 0;
    while (offset < bytes.size()) {
        auto n = ::read(random.value, bytes.data() + offset, bytes.size() - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) throw std::runtime_error("RANDOM_UNAVAILABLE");
        offset += static_cast<std::size_t>(n);
    }
    bytes[6] = (bytes[6] & 15) | 64; bytes[8] = (bytes[8] & 63) | 128;
    constexpr char hex[] = "0123456789abcdef"; std::string out;
    for (std::size_t i = 0; i < bytes.size(); ++i) { if (i == 4 || i == 6 || i == 8 || i == 10) out += '-'; out += hex[bytes[i] >> 4]; out += hex[bytes[i] & 15]; }
    return out;
}
std::string exchange_credential(const std::string& request, const std::atomic<bool>& stop) {
    sockaddr_un address{}; address.sun_family = AF_UNIX;
    std::strcpy(address.sun_path, "/run/aosedge/platform/kuksa-auth/request.sock");
    return exchange(AF_UNIX, reinterpret_cast<sockaddr*>(&address), sizeof(address), request, 32768, 8, stop);
}
HttpResponse parse_http_response(const std::string& bytes) {
    if (bytes.size() > 24576) fail();
    auto split = bytes.find("\r\n\r\n");
    if (split == std::string::npos || split > 16384) fail();
    std::istringstream headers(bytes.substr(0, split)); std::string line; std::getline(headers, line);
    if (line.size() < 13 || line.substr(0, 9) != "HTTP/1.1 " || line[12] != ' ') fail();
    HttpResponse response; response.status = static_cast<int>(number(std::string_view(line).substr(9, 3)));
    std::map<std::string, std::string> fields;
    while (std::getline(headers, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto colon = line.find(':'); if (colon == std::string::npos) fail();
        auto name = line.substr(0, colon); std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        auto value = line.substr(colon + 1); while (!value.empty() && value.front() == ' ') value.erase(0, 1);
        if (!fields.emplace(name, value).second) fail();
    }
    auto body = bytes.substr(split + 4);
    if (fields.count("content-length") && fields.count("transfer-encoding")) fail();
    if (fields.count("transfer-encoding")) {
        if (fields.at("transfer-encoding") != "chunked") fail();
        for (;;) {
            auto end = body.find("\r\n"); if (end == std::string::npos) fail();
            const auto n = number(std::string_view(body).substr(0, end), 16); body.erase(0, end + 2);
            if (n > 8192 || n > body.size() || body.size() - n < 2 || body.substr(n, 2) != "\r\n") fail();
            if (n == 0) { if (body != "\r\n") fail(); break; }
            response.body += body.substr(0, n); body.erase(0, n + 2);
            if (response.body.size() > 8192) fail();
        }
    } else {
        if (fields.count("content-length") && number(fields.at("content-length")) != body.size()) fail();
        response.body = std::move(body);
    }
    if (response.body.size() > 8192) fail();
    if (fields.count("retry-after")) {
        const auto value = number(fields.at("retry-after"));
        if (value > 86400) fail();
        response.retry_after = static_cast<int>(value);
    }
    return response;
}
HttpResponse post_backend(const std::string& bytes, const std::atomic<bool>& stop) {
    if (bytes.empty() || bytes.size() > 65536) throw std::invalid_argument("MESSAGE_SIZE_INVALID");
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(18091);
    if (::inet_pton(AF_INET, "10.0.0.1", &address.sin_addr) != 1) fail();
    const auto request = "POST /api/v1/brake/messages HTTP/1.1\r\nHost: 10.0.0.1:18091\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " + std::to_string(bytes.size()) + "\r\n\r\n" + bytes;
    return parse_http_response(exchange(AF_INET, reinterpret_cast<sockaddr*>(&address), sizeof(address), request, 24576, 10, stop));
}
}  // namespace brake_health::runtime
