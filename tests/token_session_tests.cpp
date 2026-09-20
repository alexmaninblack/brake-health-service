// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0
#include "brake_health/runtime/application.hpp"
#include "brake_health/runtime/session_reconnect.hpp"
#include "brake_health/runtime/readiness_publication.hpp"
#include <fstream>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
using namespace brake_health::runtime;
namespace fs = std::filesystem;
void check(bool ok) { if (!ok) throw std::runtime_error("TOKEN_SESSION_TEST_FAILED"); }
void rejects(const std::function<void()>& fn) {
    bool rejected = false;
    try { fn(); } catch (const std::exception&) { rejected = true; }
    check(rejected);
}
void rejects_code(const std::function<void()>& fn, const std::string& expected) {
    try { fn(); } catch (const std::exception& error) {
        check(error.what() == expected);
        return;
    }
    check(false);
}
int main() {
    // Token renewal: false can be published before the first fresh sample.
    // Recovery must not wait for the old five-second heartbeat.
    ReadinessPublication readiness;
    check(readiness.due(false, 0)); readiness.completed(false, 0, true);
    check(!readiness.due(false, 174));
    check(readiness.due(true, 174)); readiness.completed(true, 174, true);
    check(!readiness.due(true, 5173)); check(readiness.due(true, 5174));
    // Genuine loss is prompt; bursts are bounded to one write per 100 ms.
    check(readiness.due(false, 300)); readiness.completed(false, 300, true);
    check(!readiness.due(true, 399)); check(readiness.due(true, 400));
    // Denied, failed or uncertain writes never establish accepted readiness.
    readiness.completed(true, 400, false);
    check(!readiness.due(true, 1399)); check(readiness.due(true, 1400));
    readiness.completed(true, 1400, true);
    check(!readiness.due(true, 6399)); check(readiness.due(false, 1500));
    readiness.completed(true, 6400, false);
    check(!readiness.due(true, 7399)); check(readiness.due(true, 7400));
    // A new session/peer starts independently; no fabricated false if data
    // arrived first, and no inherited successful publication after restart.
    ReadinessPublication fresh;
    check(fresh.due(true, 0)); fresh.completed(true, 0, true);
    check(!fresh.due(true, 4999)); check(fresh.due(true, 5000));
    ReadinessPublication failed_initial;
    check(failed_initial.due(false, 0)); failed_initial.completed(false, 0, false);
    check(!failed_initial.due(true, 999)); check(failed_initial.due(true, 1000));
    const auto pending = subscription_failure_observation("KUKSA_AUTH_PENDING");
    check(std::string(pending.connection) == "STARTING" && std::string(pending.input) == "WAITING" &&
          std::string(pending.reason) == "AWAITING_INPUT");
    const auto denied = subscription_failure_observation("KUKSA_AUTH_UNAVAILABLE");
    check(std::string(denied.connection) == "ACCESS_DENIED" && std::string(denied.input) == "ACCESS_DENIED");
    check(std::string(subscription_failure_observation("KUKSA_DATA_UNAVAILABLE").input) == "DISCONNECTED");
    check(std::string(subscription_failure_observation("VDP_INCOMPATIBLE").input) == "INVALID");
    auto name = (fs::canonical(fs::temp_directory_path()) / "token-session-tests-XXXXXX").string();
    check(::mkdtemp(name.data()) != nullptr);
    const fs::path root(name);
    struct Cleanup { fs::path p; ~Cleanup() { fs::remove_all(p); } } cleanup{root};
    rejects([&] { TokenSession bad(root); }); // Owner-only legacy root is not the new contract.
    check(::chmod(root.c_str(), 01777) == 0);
    fs::path first;
    {
        TokenSession a(root), b(root);
        first = a.token_file();
        check(first != b.token_file());
        struct stat info{};
        check(::lstat(first.parent_path().c_str(), &info) == 0);
        check((info.st_mode & 07777) == 0700 && info.st_uid == ::geteuid() && info.st_gid == ::getegid());
        rejects_code([&] { read_private_token(first); }, "KUKSA_AUTH_PENDING");
        atomic_private_file(first, "e30.e30.c2ln");
        check(read_private_token(first) == "e30.e30.c2ln");
        atomic_private_file(first, "malformed");
        rejects_code([&] { read_private_token(first); }, "KUKSA_AUTH_UNAVAILABLE");
        atomic_private_file(first, "e30.e30.c2ln");
        // Host-only fixture bytes, not an issued credential or trust chain.
        const ApplicationInputs inputs{first.parent_path() / "metadata.json", first.parent_path() / "trust.pem"};
        const auto put = [](const fs::path& path, const std::string& bytes) {
            std::ofstream file(path); file << bytes; file.close();
            check(file.good());
        };
        put(inputs.metadata_file, "metadata");
        put(inputs.ca_file, "trust");
        const auto change = [&] {
            return inspect_session_inputs(inputs, first, "e30.e30.c2ln", "metadata", "trust");
        };
        check(change() == SessionInputChange::None);
        atomic_private_file(first, "e30.e30.bmV3");
        check(change() == SessionInputChange::TokenReplaced);
        put(inputs.metadata_file, "changed");
        check(change() == SessionInputChange::Unavailable);
        put(inputs.metadata_file, "metadata");
        put(inputs.ca_file, "changed");
        check(change() == SessionInputChange::Unavailable);
        fs::remove(inputs.ca_file);
        check(change() == SessionInputChange::Unavailable);
        put(inputs.ca_file, "trust");
        a.remove_token();
        check(change() == SessionInputChange::Unavailable);
        atomic_private_file(first, "e30.e30.bmV3");
        check(::chmod(first.c_str(), 0600) == 0);
        check(change() == SessionInputChange::Unavailable);
        rejects_code([&] { read_private_token(first); }, "KUKSA_AUTH_UNAVAILABLE");
        check(::chmod(first.c_str(), 0400) == 0);
        check(change() == SessionInputChange::TokenReplaced);
        SessionInterruption planned;
        planned.observe(SessionInputChange::None);
        check(!planned.cancelled());
        planned.observe(SessionInputChange::TokenReplaced);
        planned.observe(SessionInputChange::None);
        check(planned.cancelled() && planned.token_replaced());
        planned.observe(SessionInputChange::Unavailable);
        planned.observe(SessionInputChange::TokenReplaced);
        check(planned.cancelled() && !planned.token_replaced());
        SessionInterruption missing;
        missing.observe(SessionInputChange::Unavailable);
        missing.observe(SessionInputChange::TokenReplaced);
        check(!missing.token_replaced());
        fs::remove(inputs.metadata_file);
        fs::remove(inputs.ca_file);
        atomic_private_file(first, "e30.e30.c2ln");
        rejects([&] { read_private_token(b.token_file()); });
        atomic_private_file(first, "e30.e30.bmV3");
        check(read_private_token(first) == "e30.e30.bmV3");
        const auto alias = first.parent_path() / "alias.jwt";
        fs::create_hard_link(first, alias);
        rejects([&] { read_private_token(first); });
        fs::remove(alias);
        check(::chmod(first.c_str(), 0600) == 0);
        rejects([&] { read_private_token(first); });
        check(::chmod(first.c_str(), 0400) == 0);
        check(::chmod(first.parent_path().c_str(), 0755) == 0);
        rejects([&] { read_private_token(first); });
        check(::chmod(first.parent_path().c_str(), 0700) == 0);
        fs::create_directory_symlink(first.parent_path(), root / "parent-link");
        rejects([&] { read_private_token(root / "parent-link/token.jwt"); });
        fs::remove(root / "parent-link");
        a.remove_token();
        a.remove_token(); // Expiry/terminal denial is idempotent and private.
        rejects_code([&] { read_private_token(first); }, "KUKSA_AUTH_PENDING");
        fs::create_symlink(b.token_file(), first);
        rejects([&] { read_private_token(first); });
        a.remove_token();
        atomic_private_file(b.token_file(), "e30.e30.c2ln");
        check(read_private_token(b.token_file()) == "e30.e30.c2ln");
    }
    check(!fs::exists(first.parent_path()));
    check(fs::is_empty(root));
    {
        TokenSession restarted(root);
        check(restarted.token_file() != first);
        check(!fs::exists(restarted.token_file()));
    }
    // Orphans are counted, never adopted, and never removed by another bootstrap.
    for (int n = 0; n < 8; ++n) fs::create_directory(root / ("session-orphan" + std::to_string(n)));
    rejects([&] { TokenSession exhausted(root); });
    check(std::distance(fs::directory_iterator(root), fs::directory_iterator()) == 8);
    ::unsetenv("KUKSA_TOKEN_FILE");
    rejects([] { token_file_from_environment(); });
    for (const char* bad : {"/run/aosedge/secrets/kuksa/token.jwt",
            "/storage/token.jwt", "/run/aosedge/secrets/kuksa/session-../token.jwt",
            "/run/aosedge/secrets/kuksa/session-AAAAAA/../token.jwt"}) {
        check(::setenv("KUKSA_TOKEN_FILE", bad, 1) == 0);
        rejects([] { token_file_from_environment(); });
    }
    ::unsetenv("KUKSA_TOKEN_FILE");
    std::cout << "PASS private token session creation, renewal, isolation, negatives, cleanup and bounds\n";
}
