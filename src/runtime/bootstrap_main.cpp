// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0
#include "brake_health/runtime/application.hpp"
#include "brake_health/runtime/demo_no_telemetry.hpp"
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
        const bool demo_no_telemetry = argc > 1 && std::string(argv[argc - 1]) == "--demo-no-telemetry";
        if (demo_no_telemetry) --argc;
        auto inputs = parse_arguments(argc, argv);
        initialize_service_inputs(inputs);
        const auto metadata = runtime_metadata(inputs, read_file(inputs.metadata_file, 8192));
        if (demo_no_telemetry) return run_demo_no_telemetry(metadata);
        (void)read_file(inputs.ca_file, 65536);
        const char* environment_secret = std::getenv("AOS_SECRET");
        if (!environment_secret || !*environment_secret) throw std::runtime_error("AOS_SECRET_UNAVAILABLE");
        const std::string request = credential_request(environment_secret);
        if (::unsetenv("AOS_SECRET") != 0) throw std::runtime_error("CREDENTIAL_ENVIRONMENT_INVALID");
        ::umask(0077);
        TokenSession session;
        if (::setenv("KUKSA_TOKEN_FILE", session.token_file().c_str(), 1) != 0)
            throw std::runtime_error("CREDENTIAL_ENVIRONMENT_INVALID");
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
                child = -1; stop = true; session.remove_token();
                return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
            }
            // This runs independently of the in-flight eight-second KAC call.
            if (has_token && lease.expired(wall, boot)) { session.remove_token(); has_token = false; auth_state(false); }
            if (issue.valid() && issue.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
                std::optional<Credential> received;
                try {
                    received = issue.get();
                } catch (...) { next_attempt = boot + retry_delay(failures++, jitter(random)) * 1000; }
                if (received) {
                    const auto& credential = *received;
                    if (!credential.token.empty()) {
                        if (credential.expires <= wall || credential.renew_after <= wall) throw std::runtime_error("KAC_RESPONSE_EXPIRED");
                        atomic_private_file(session.token_file(), credential.token);
                        lease.issued(credential, wall, boot);
                        next_attempt = lease.renew_boot; failures = 0;
                        if (!has_token) auth_state(true);
                        has_token = true;
                    } else if (!credential.retryable) {
                        session.remove_token(); terminal = true;
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
        session.remove_token();
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
