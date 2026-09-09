// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0
#include "brake_health/runtime/application.hpp"
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <future>
#include <iostream>
#include <random>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

namespace {
static_assert(std::atomic<bool>::is_always_lock_free, "Signal flag must be lock-free");
std::atomic<bool> interrupted{false};
void signal_handler(int) { interrupted.store(true, std::memory_order_relaxed); }
using namespace brake_health::runtime;
void auth_state(bool ready) {
    std::cout << "{\"schemaVersion\":1,\"eventType\":\"KUKSA_AUTH_CHANGED\",\"severity\":\"INFO\",\"currentState\":\""
              << (ready ? "READY" : "NOT_READY") << "\",\"reasonCode\":\""
              << (ready ? "NONE" : "KUKSA_AUTH_UNAVAILABLE") << "\"}" << std::endl;
}
void remove_token() {
    if (::unlink(token_path) != 0 && errno != ENOENT) throw std::runtime_error("TOKEN_REMOVE_FAILED");
}
void stop_child(pid_t child) {
    ::kill(child, SIGTERM);
    for (int i = 0; i < 50; ++i) {
        int status = 0;
        const auto result = ::waitpid(child, &status, WNOHANG);
        if (result == child || (result < 0 && errno == ECHILD)) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ::kill(child, SIGKILL);
    while (::waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
}
}
int main(int argc, char** argv) {
    std::atomic<bool> stop{false};
    pid_t child = -1;
    try {
        const auto inputs = parse_arguments(argc, argv);
        (void)parse_metadata(read_file(inputs.metadata_file, 8192));
        (void)read_file(inputs.ca_file, 65536);
        const char* environment_secret = std::getenv("AOS_SECRET");
        if (!environment_secret || !*environment_secret) throw std::runtime_error("AOS_SECRET_UNAVAILABLE");
        const std::string request = credential_request(environment_secret);
        if (::unsetenv("AOS_SECRET") != 0 || ::setenv("KUKSA_TOKEN_FILE", token_path, 1) != 0) throw std::runtime_error("CREDENTIAL_ENVIRONMENT_INVALID");
        ::umask(0077);
        struct stat directory{};
        const auto token_directory = std::filesystem::path(token_path).parent_path();
        if (::lstat(token_directory.c_str(), &directory) != 0 || !S_ISDIR(directory.st_mode) ||
            directory.st_uid != ::geteuid() || (directory.st_mode & 0777) != 0700) throw std::runtime_error("TOKEN_DIRECTORY_INVALID");
        remove_token();
        std::signal(SIGINT, signal_handler); std::signal(SIGTERM, signal_handler);
        const auto bootstrap_pid = ::getpid();
        child = ::fork();
        if (child < 0) throw std::runtime_error("ANALYTICS_START_FAILED");
        if (child == 0) {
#ifdef __linux__
            // No orphaned stream may outlive the process owning token expiry.
            if (::prctl(PR_SET_PDEATHSIG, SIGTERM) != 0 || ::getppid() != bootstrap_pid) ::_exit(127);
#else
            (void)bootstrap_pid;  // macOS host compile only; product target is Linux.
#endif
            ::execl("/usr/bin/brake-health-service", "brake-health-service", "--metadata-file", inputs.metadata_file.c_str(),
                    "--ca-file", inputs.ca_file.c_str(), static_cast<char*>(nullptr));
            ::_exit(127);
        }
        Lease lease;
        bool has_token = false, terminal = false;
        std::int64_t next_attempt = 0;
        unsigned failures = 0;
        std::mt19937 random(std::random_device{}());
        std::uniform_real_distribution<double> jitter(-0.2, 0.2);
        std::future<Credential> issue;
        auth_state(false);
        while (!interrupted) {
            const auto boot = boot_milliseconds(), wall = wall_milliseconds() / 1000;
            int status = 0;
            if (::waitpid(child, &status, WNOHANG) == child) {
                child = -1; stop = true; remove_token();
                return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
            }
            // This runs independently of the in-flight eight-second KAC call.
            if (has_token && lease.expired(wall, boot)) { remove_token(); has_token = false; auth_state(false); }
            if (issue.valid() && issue.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
                std::optional<Credential> received;
                try {
                    received = issue.get();
                } catch (...) { next_attempt = boot + retry_delay(failures++, jitter(random)) * 1000; }
                if (received) {
                    const auto& credential = *received;
                    if (!credential.token.empty()) {
                        if (credential.expires <= wall || credential.renew_after <= wall) throw std::runtime_error("KAC_RESPONSE_EXPIRED");
                        atomic_private_file(token_path, credential.token);
                        lease.issued(credential, wall, boot);
                        next_attempt = lease.renew_boot; failures = 0;
                        if (!has_token) auth_state(true);
                        has_token = true;
                    } else if (!credential.retryable) {
                        remove_token(); terminal = true;
                        if (has_token) auth_state(false);
                        has_token = false;
                    } else next_attempt = boot + retry_delay(failures++, jitter(random)) * 1000;
                }
            }
            if (!issue.valid() && !terminal && boot >= next_attempt) {
                issue = std::async(std::launch::async, [&] {
                    return parse_credential(exchange_credential(request, stop), wall_milliseconds() / 1000);
                });
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        stop = true;
        remove_token();
        stop_child(child); child = -1;
        return 0;
    } catch (...) {
        stop = true;
        if (child > 0) stop_child(child);
        // No external error text, secret, token, certificate or protocol frame.
        std::cerr << "{\"schemaVersion\":1,\"eventType\":\"KUKSA_AUTH_CHANGED\",\"severity\":\"ERROR\",\"currentState\":\"NOT_READY\",\"reasonCode\":\"KUKSA_AUTH_UNAVAILABLE\"}" << std::endl;
        return 2;
    }
}
