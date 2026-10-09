<!-- SPDX-FileCopyrightText: 2026 maninblack -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Brake Health Service

Independent in-vehicle Brake Health QM service. It consumes authorized vehicle
data, produces analytics/advisory and does not control braking.

## SDV Lab entry

For the complete demo, start at the
[SDV Lab product repository](https://github.com/alexmaninblack/aosedge-sdv-demo).
Operators use its prebuilt installer; developers use its pinned build route.
This component is not a standalone installer for the whole lab. Integration
source pins and published artifact provenance do not change when this README
changes. Detailed historical evidence below retains its original scope.

[Component architecture](docs/architecture.md), [runtime profiles](docs/runtime-profiles.md)
and [product build](docs/product-build.md).

Local checks: `python3 -B tools/quality_gate.py` and the Python test command
below. Tests can compile C++ into temporary directories; select SSD scratch.
Use product-build instructions for real ARM64 output, not the historical scaffold.


## Current integration evidence — 7 October 2026

Normal packages use real KUKSA telemetry and native Aos identity/permissions.
The [Kit028 source return point](../aosedge-sdv-demo/docs/qualification/kit028-setup042-source-publication-2026-10-05.md)
binds this implementation. The installed M1 run on Factory .41 exercised
Brake112/113/114 (V1/V2/V3), independent Reset/history, offline backlog delivery
and new products after same-identity ignition. Current source includes input
and credential-renewal continuity corrections. These are dated scripted
results, not current runtime observations or a completed native E2E verdict.

The [current baseline](../aosedge-sdv-demo/docs/qualification/current-baseline.md)
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
