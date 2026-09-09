<!-- SPDX-FileCopyrightText: 2026 maninblack -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Brake Health Service

Independently deployable AosEdge-managed service consuming versioned KUKSA/VSS
vehicle telemetry for on-board brake-health analysis.

## Status

This repository contains an R-3 diagnostic ARM64 Aos service scaffold plus
source-complete, dependency-free C++17 Brake Health v1 and v2 domain cores.
The v1 core implements the accepted six-signal validation, deterministic event
window, canonical logical messages and bounded local spool. The v2 core accepts
an already completed fixed-point 12-signal episode, runs the accepted synthetic
condition model, emits closed canonical assessment/band-change messages and
persists crash-safe exactly-once state plus a bounded derived-message outbox.

A bounded v1 runtime library now composes the domain engine and durable spool,
with coherent-frame admission, KAC envelope validation, bounded POSIX transport
primitives and exact backend receipt handling. See the
[runtime increment](docs/runtime-increment.md) and the subsequent
[executable candidate and deployment gates](docs/runtime-executable.md).
The credential bootstrap compiles on the host. The real TLS KUKSA Get/Subscribe
and asynchronous delivery executable is implemented behind an explicit product
build target requiring pinned C++ dependencies; that target has not yet been
compiled or exercised in this increment.
The packaged scaffold executable remains unchanged: it contains none of this
runtime and no KUKSA subscription or bootstrap process. Source completion does
not claim a composed, deployable, calibrated or qualified Brake Health Service.

The [Linux ARM64 product build recipe](docs/product-build.md) is ready for
Demo Control to invoke. Its export requires the actual gRPC executable,
successful CTest evidence and verified ELF dependency closure; it never
substitutes the diagnostic scaffold.

## Ownership Boundary

This repository owns a cloud-managed application with an independent Aos
service/SOTA lifecycle. The service will consume a published vehicle-data
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
credentials or network input. A future runtime adapter will use the platform-
owned fixed-resource KAC exchange: the Service supplies only its instance-
bound `AOS_SECRET`, while resource `kuksa` and all authority remain implicit
and derived from current Aos IAM state.

## Current Scaffold

The scaffold declares:

- vehicle telemetry contract compatibility `>=0.1.0, <0.2.0`;
- KUKSA API `kuksa.val.v1` through the read-only Aos resource `kuksa`;
- an `arm64` Aos service image and explicit CPU, RAM, storage, state, temporary
  storage, file, and process limits;
- no Aos layer dependency and no CARLA, VISS, provider, or VM integration.

Build an unsigned local staging directory with:

```text
python3 tools/build_scaffold.py --output build/aos-service-scaffold
```

The output deliberately excludes signing and TLS credentials. Run all local
gates with:

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
