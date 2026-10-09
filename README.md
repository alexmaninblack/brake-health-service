<!-- SPDX-FileCopyrightText: 2026 maninblack -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Brake Health Service

Independent in-vehicle Brake Health QM service. It consumes authorized telemetry, produces analytics/advisory, and does not control braking.

<a id="sdv-lab-entry"></a>

For the **whole demo**, start at the [SDV Lab README](https://github.com/alexmaninblack/aosedge-sdv-demo).
Only the product repository is manually cloned for its pinned multi-component
build. The instructions below are for working on **this component alone**;
a host check does not publish, install or qualify a vehicle package.

## 1 Prepare a macOS component workspace

Use native Apple Silicon Terminal. These component commands are for development,
not a qualified full-demo installation. Run blocks in order and stop on error.
The revised instructions await the joint walkthrough; they were not executed
during this documentation update.

Choose an already mounted external APFS SSD:

```sh
uname -m
printf 'Mounted external APFS volume (for example /Volumes/BUILD): '
read -r SDV_VOLUME
diskutil info "$SDV_VOLUME"
df -h "$SDV_VOLUME"
```

Expect `arm64` and the actual external volume. Do not create a missing mount
directory. After confirming storage:

```sh
SDV_WORK="$SDV_VOLUME/sdv-components"
mkdir -p "$SDV_WORK" "$SDV_VOLUME/tmp"
export TMPDIR="$SDV_VOLUME/tmp"
export HOMEBREW_CACHE="$SDV_WORK/cache/homebrew"
```

Install Apple's Command Line Tools with `xcode-select --install` if missing,
and finish the system dialog. Install [Homebrew](https://docs.brew.sh/Installation)
if absent. Then:

```sh
eval "$(/opt/homebrew/bin/brew shellenv)"
brew install cmake python@3.12
export PATH="$(brew --prefix python@3.12)/libexec/bin:$PATH"
git --version
cmake --version
python3 --version
xcrun clang++ --version
```

Do not use the installed demo's private interpreter or a Rosetta toolchain.

## 2 Clone this component

```sh
git clone --branch main https://github.com/alexmaninblack/brake-health-service.git "$SDV_WORK/brake-health-service"
cd "$SDV_WORK/brake-health-service"
git rev-parse HEAD
```

Record the printed revision with your results. `main` is current development,
not a release pin. To reproduce the complete candidate, use the product
repository's manifest-driven route instead of independently choosing branches.

## 3 Build and check host targets

```sh
SDV_BUILD="$SDV_WORK/build/brake-health-service"
cmake -S . -B "$SDV_BUILD" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DBHS_BUILD_KUKSA_RUNTIME=OFF
cmake --build "$SDV_BUILD" --parallel 2
ctest --test-dir "$SDV_BUILD" --output-on-failure
```

Expect a successful build and no failed CTest cases. Keep the first failure;
do not replace expected results or lower resource/security requirements.

This C++17 build creates host/domain libraries, bootstrap and test programs.
It deliberately **does not build the real KUKSA service executable**. The
configuration reports that distinction; a bootstrap is not a deployable product.

For the repository's packaging/policy and additional host checks:

```sh
python3 -B -m unittest discover -s tests -p 'test_*.py'
python3 -B tools/quality_gate.py
```

The Python suite can compile additional temporary C++ fixtures under
`TMPDIR`. It is not a no-build test command.

## 4 Produce the vehicle package

Use the product repository's pinned build chain or its Demo Control product
builder, following [Linux ARM64 product build](docs/product-build.md).
That route supplies the pinned gRPC/Protobuf/KUKSA dependencies and exports the
actual executable. Do not sign the historical diagnostic scaffold or run the
bootstrap on macOS with invented vehicle identity.

Brake profiles V1, V2 and V3 are functional content, distinct from allocated
release numbers. The integration owner signs, publishes and assigns them
serially; this component quickstart deploys nothing.

## 5 Finish

Build and test commands exit on completion; no long-running vehicle process
was started. Preserve results/source revision. Do not delete the installed
service's storage, keys or Test state to clean a host build.

## Component documentation

- [Architecture](docs/architecture.md)
- [Runtime profiles](docs/runtime-profiles.md)
- [Product build](docs/product-build.md)
- [Deployment gates](docs/runtime-executable.md)
- [Security](SECURITY.md), [license](LICENSE), [notices](NOTICE)

## Implementation reference and dated evidence

The material below preserves detailed contracts, milestones and specialist
examples. Historical commands are not the first-use sequence above. Original
qualification dates/scope remain unchanged by this documentation revision.

<details>
<summary>Expand implementation reference and historical evidence</summary>

## Current integration evidence — 7 October 2026

Normal packages use real KUKSA telemetry and native Aos identity/permissions.
The [Kit028 source return point](https://github.com/alexmaninblack/aosedge-sdv-demo/blob/5e30b410cbeadd2f73063b1bd313253aff612595/docs/qualification/kit028-setup042-source-publication-2026-10-05.md)
binds this implementation. The installed M1 run on Factory .41 exercised
Brake112/113/114 (V1/V2/V3), independent Reset/history, offline backlog delivery
and new products after same-identity ignition. Current source includes input
and credential-renewal continuity corrections. These are dated scripted
results, not current runtime observations or a completed native E2E verdict.

The [current baseline](https://github.com/alexmaninblack/aosedge-sdv-demo/blob/5e30b410cbeadd2f73063b1bd313253aff612595/docs/qualification/current-baseline.md)
retains the remaining native, calibration, nonempty-outbox power-loss and fault
gates. Load-sensitive brief readiness is deferred; no freshness threshold was
relaxed. Normal packages request 250 DMIPS, 1024 open files and 24 PIDs.
Requests are not measured usage or proof of all resource enforcement cases.

## Historical opt-in Test-only lifecycle mode

The explicit final bootstrap argument `--demo-no-telemetry` is a temporary
Cloud-permissions workaround accepted on 11 September 2026. It validates native
identity, packaged version and public metadata, rejects Production, and keeps
only the bootstrap alive until SIGTERM/SIGINT. It does not start analytics,
request a token, connect to KUKSA, emit derived records or produce advisory.
Its single lifecycle event reports `NOT_READY / TELEMETRY_DISABLED`.
It is never selected automatically when authorization fails.

Build, package and publish through Demo Control. Preparation requires both
`--without-permissions --demo-no-telemetry`; this explicit package mode also
requests `noFileLimit: 1024` for native container construction. That file limit
also applies to normal telemetry-enabled packages; the workaround does not
authorize fallback from failed permissions.

Independently deployable AosEdge-managed service consuming versioned KUKSA/VSS
vehicle telemetry for on-board brake-health analysis.

## Status

This repository contains C++17 Brake Health domain cores and an explicit
v1/v2/v3 product executable, alongside a historical R-3 diagnostic scaffold.
The v1 core implements the accepted six-signal validation, deterministic event
window, canonical logical messages and bounded local spool. The v2 core accepts
an already completed fixed-point 12-signal episode, runs the accepted synthetic
condition model, emits closed canonical assessment/band-change messages and
persists crash-safe exactly-once state plus a bounded derived-message outbox.

The [product runtime profiles](docs/runtime-profiles.md) compose live signal
admission, v1 growing windows, v2 durable model processing and v3 correlated
advisory requests/facts. Release numbers and functional profiles are separate.
The credential bootstrap uses the accepted KAC exchange; the child uses
authenticated TLS KUKSA and independent durable backend delivery.

Demo Control's integration owner reported a successful real Linux ARM64/gRPC
v1 build at source `81e6afc1563239c8d75fb7183a50546cde7ddc58`.
Subsequent recovery/readiness corrections received their own scoped product
build/live proof as linked above; the older build is not the latest checkpoint.
Neither source tests nor ELF compilation prove Service-identity permissions,
node-rootfs compatibility, transport, resource quotas or deployment success.
See the [explicit deployment gates](docs/runtime-executable.md).
The historical packaged scaffold remains diagnostic-only and must not be
signed or uploaded as the product executable.

The [Linux ARM64 product build recipe](docs/product-build.md) is ready for
Demo Control to invoke. Its export requires the actual gRPC executable,
successful CTest evidence and verified ELF dependency closure; it never
substitutes the diagnostic scaffold.

## Ownership Boundary

This repository owns a cloud-managed application with an independent Aos
service/SOTA lifecycle. The service consumes a published vehicle-data
contract through KUKSA and must remain independent of:

- CARLA libraries and endpoints;
- VISS transport handling;
- CAN, SOME/IP, DDS, or OEM provider implementations;
- AosVM host launch and provisioning code;
- the source layout of `aos-vehicle-platform`.

The repository owns one application product that evolves through immutable
v1, v2 and v3 compositions. It owns application behavior, tests, Aos service
packaging, resource limits, version compatibility, health reporting and
rollbackable release metadata without splitting those versions into separate
products.

The v1 and v2 domain libraries accept no simulator truth, control mode,
credentials or network input. The runtime adapter uses the platform-
owned fixed-resource KAC exchange: the Service supplies only its instance-
bound `AOS_SECRET`, while resource `kuksa` and all authority remain implicit
and derived from current Aos IAM state.

## Historical Diagnostic Scaffold

The scaffold declares:

- vehicle telemetry contract compatibility `>=0.1.0, <0.2.0`;
- KUKSA API `kuksa.val.v1` through the read-only Aos resource `kuksa`;
- an `arm64` Aos service image and explicit CPU, RAM, storage, state, temporary
  storage, file, and process limits;
- no Aos layer dependency and no CARLA, VISS, provider, or VM integration.

Its packaging library also owns the real product export, invoked through
Demo Control as described in the product build contract. Do not substitute
the diagnostic staging path for product preparation. Run source-only gates with:

```text
python3 -m unittest discover -s tests -p 'test_*.py'
python3 tools/quality_gate.py
```

The Python suite configures and builds the C++17 domain and runtime libraries
in temporary out-of-tree directories and runs their deterministic CTest suites.
The v2 suite covers the authoritative golden assessment/event identities and
digests, closed input-quality outcomes, journal recovery, atomic pair overflow,
durable-ACK deletion, verified duplicate identity, every persistence write
stage, identity-ledger recovery, ledger rollover and coexistence with retained v1 spool bytes. It
downloads no dependency and does not alter the packaged scaffold executable.

## Security and Secrets

Do not commit private keys, KUKSA access tokens, user certificates, signing
credentials, device identities, cloud account material, VM artifacts, or raw
operational logs. See [SECURITY.md](SECURITY.md).

## License

Original project work is licensed under the Apache License, Version 2.0, with
copyright held under the exact name `maninblack`. Third-party material retains
its own license and notices. See [LICENSE](LICENSE), [NOTICE](NOTICE), and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

</details>
