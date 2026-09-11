// SPDX-FileCopyrightText: 2026 maninblack
// SPDX-License-Identifier: Apache-2.0
#include "brake_health/runtime/application.hpp"
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
int main() {
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
        rejects([&] { read_private_token(first); });
        atomic_private_file(first, "e30.e30.c2ln");
        check(read_private_token(first) == "e30.e30.c2ln");
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
