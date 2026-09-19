<!-- SPDX-FileCopyrightText: 2026 maninblack -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Linux ARM64 product build and export contract

The root `Dockerfile` is a **preparation-only build recipe**, not a runtime
container. All actual builds/exports are invoked by Demo Control. It builds
the real `brake-health-bootstrap` and `brake-health-service` with
`BHS_BUILD_KUKSA_RUNTIME=ON`; the diagnostic scaffold is never substituted.
No VM, Cloud, Builder, Docker pull/build/start, signing or upload was performed
while preparing this source increment.

## Canonical invocation owned by Demo Control

Demo Control resolves a clean source checkout, exact commit and its commit
timestamp, then invokes the following argument vector (no shell interpolation):

```text
docker buildx build
  --platform linux/arm64
  --file <brake-health-service>/Dockerfile
  --target export
  --build-arg SOURCE_REVISION=<40-character source commit>
  --build-arg SOURCE_DATE_EPOCH=<positive commit Unix timestamp>
  --build-arg BHS_FUNCTIONAL_PROFILE=<v1|v2|v3>
  --build-arg BUILD_JOBS=4
  --output type=local,dest=<owned-output-directory>
  <brake-health-service>
```

`BUILD_JOBS` accepts only 1 through 8; default is 4. `SOURCE_REVISION` and
`SOURCE_DATE_EPOCH` are mandatory and have no invented fallback. The output
directory belongs to that build attempt; Demo Control retains its outer
`build.json` and never merges a failed export into an accepted candidate.
The `.dockerignore` allowlist excludes Git, local state, keys, identities,
artifacts and deployment credentials from the build context.
The functional profile is explicit and independent of the release version;
Demo Control catalogs each source/profile pair separately.

The first preparation build compiles dependency layers. Later source changes
reuse those immutable Docker cache layers. There is no dependency rebuild,
image pull or network installation during a demo deployment action.

## Pinned inputs

- Official Debian bookworm Linux ARM64 base:
  `sha256:6bd27d44e6c32a66bbd72d7cb2b76a8ae3497ec2e5274a81abd1b37f6013fa1f`.
  Toolchain package selection uses the Debian/debian-security snapshots at
  `20260901T000000Z`; package signatures remain checked. Only the archive's
  historical metadata expiry check is disabled. Export records the GCC
  runtime package versions actually installed.
- gRPC `e5ae3b6b44bf3b64d24bfb4b4f82556239b986db` (1.60.1), Protobuf
  `a4cbdd3ed0042e8f9b9c30e8b0634096d9532809` (4.25.8/CMake 25.8.0), Abseil
  `54fac219c4ef0bc379dfffb0b8098725d77ac81b` (20240116.3), KUKSA
  `30e5c13abc496d0b39aaa6c25acebb088b9902e3` (0.5.0).
- OpenSSL 3.2.6 archive SHA-256
  `89681a9ddaa9ed7cf25ea8ef61338db805200bae47d00510490623547380c148`, matching
  the accepted platform dependency inventory. Its static build does not load
  external OpenSSL provider modules.
- c-ares, RE2, zlib and upstream protocol sources are fetched by the exact
  gRPC gitlinks, not branch/tag heads. Their actual revisions are checked and
  emitted in the product manifest. Unused BoringSSL/benchmark/test submodules
  are not fetched. Required protocol submodules are present before gRPC
  configuration, so its implicit missing-source downloads are not needed.
- KUKSA `val.proto` and `types.proto` revision and content hashes are checked
  by CMake before generation. Generated code remains in the build directory.

The pinned recipe is reproducible in its inputs. Bit-for-bit repeat-build
identity has not yet been demonstrated; test timing evidence can naturally
differ across builds and is not part of executable identity.

## Runtime dependency closure

gRPC, Protobuf, Abseil, OpenSSL, c-ares, RE2, zlib, libstdc++ and libgcc are
linked statically. The two product ELFs remain dynamically linked to glibc;
the exporter permits only `libc.so.6`, `libm.so.6` and
`ld-linux-aarch64.so.1` in `DT_NEEDED`. Both must be AArch64 ELF64 little-endian
executables with interpreter `/lib/ld-linux-aarch64.so.1`, no RPATH/RUNPATH,
and no required GLIBC symbol newer than 2.36. The actual lists are recorded,
not assumed from compiler flags.

The transient Test proof must verify that the existing guest satisfies this
loader/glibc boundary. No guest gRPC/Protobuf C++ ABI is assumed or installed.
Neither a guest library replacement nor a Factory image rebuild is included
in this recipe. Thread, memory and CPU quotas remain live qualification work.

Public upstream LICENSE/NOTICE/COPYING/COPYRIGHT/AUTHORS files are preserved by
relative source path. GCC runtime copyright and runtime-exception notices are
included because libstdc++/libgcc are statically linked. The output contains no
TLS trust leaf, private key, JWT, runtime metadata, current UID or test records.

## Exact output

```text
output/
  product-build.json
  evidence/ctest-results.xml
  rootfs/usr/bin/brake-health-bootstrap
  rootfs/usr/bin/brake-health-service
  rootfs/usr/share/licenses/brake-health-service/...
```

The internal product-export mode lives in the existing packaging library
`tools/build_scaffold.py`; it is invoked only inside the Docker recipe. It
verifies real ELF headers and `readelf` evidence, source revisions, OpenSSL
archive hash and the actual successful CTest JUnit report before export. It
does not interpret an `arm64` directory name as architecture evidence.

`product-build.json` fields:

| Field | Value / meaning |
| --- | --- |
| `schemaVersion` | Integer `1` |
| `kind` | `brake-health-linux-arm64-product` |
| `sourceRevision`, `sourceDateEpoch` | Exact clean-source commit and timestamp supplied by Demo Control |
| `architecture`, `os` | `arm64`, `linux` |
| `productTarget` | `BHS_BUILD_KUKSA_RUNTIME=ON` |
| `functionalProfile` | Explicit `v1`, `v2` or `v3`, never inferred from a release number |
| `binaries` | Exactly two entries: `path` relative to output, `sha256`, `size`, `interpreter`, `needed`, `glibcVersions` |
| `tests` | `ctest: passed`, `count: 8`, report path and `reportSha256`; missing/failed/skipped suites refuse export |
| `dependencies` | Verified git source pins and OpenSSL archive digest |
| `compilerRuntimePackages` | Exact installed GCC runtime package versions |
| `baseImage`, `aptSnapshot` | Immutable build environment references |
| `liveQualified` | Always `false`; compilation is not live qualification |

The eight required CTest targets cover native package/public/identity inputs,
private token sessions, v1/v2 domains, runtime protocol/delivery and composed
v1/v2/v3 application boundaries, mock isolation and durable function observation
delivery. The actual gRPC executable is a mandatory build target, but these
tests do not claim a KAC/TLS subscription or live service E2E. Those proofs must
follow through Demo Control with the approved public-trust/metadata binding.

Successful build output is still unsigned and contains no Aos deployment
configuration. Demo Control must assemble it with the accepted per-service
configuration and runtime-resource bindings before signing/publication.
