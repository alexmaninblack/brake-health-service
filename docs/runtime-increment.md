<!-- SPDX-FileCopyrightText: 2026 maninblack -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Studio P5: bounded v1 runtime increment

Status: host-tested source library, **not a deployable SOTA service**. This is
the original bounded increment record. The subsequent executable candidate,
current test scope and unresolved integration gates are recorded in
[runtime-executable.md](runtime-executable.md); that record supersedes the
"not implemented yet" composition statements below.

This increment composes the existing v1 domain engine and persistent spool.
It introduces no new product payload schema, simulator input, credentials in
the package, background workflow wrapper, or platform permission. The existing
diagnostic package is deliberately unchanged: it must not be published as the
Brake v1 product runtime.

## Implemented boundary

| Source | Implemented behavior | Evidence boundary |
| --- | --- | --- |
| `runtime/runtime.cpp` | Six-path frame assembly rejects missing, mixed-timestamp, stale, future, nonfinite and nonintegral pedal values; the existing domain validator still owns physical bounds | Caller supplies observations; no KUKSA subscription exists yet |
| `runtime/runtime.cpp`, `v1/spool.cpp` | Active-window restart checkpoints, source-gap/service-stop completion, terminal publication without duplicate completion-byte accounting, recovery, exact durable receipt matching and ACK-only deletion | Local persistent-file tests; no guest or backend process contacted |
| `runtime/json.cpp` | Bounded strict JSON envelopes, duplicate-key rejection, depth/UTF-8/number limits | Protocol parser, not a replacement for existing canonical product serialization |
| `runtime/runtime.cpp` | Fixed-resource KAC issue request and issued/rejected response validation; 300-second lifetime/180-second renewal profile | Does not authenticate a JWT itself; KAC issues and KUKSA verifies tokens |
| `runtime/posix.cpp` | Bounded cancellable AF_UNIX exchange, private atomic file replacement, fixed isolated Brake HTTP POST, strict bounded HTTP response parsing | Compiled transport primitives; live socket exchange and process bootstrap are not qualified |
| `runtime/runtime.cpp` | Separate acquisition/delivery caller interface; HTTP I/O is outside the queue lock; bounded retry calculation; retry keeps identical bytes; permanent conflicts retain/quarantine | No delivery thread/scheduler or readiness publisher is started by this library |

`Runtime` uses explicit injected `MessageMetadata`. Test identities and frames
exist only in the host test suite. They are not defaults, product data or a
deployment configuration source. This increment does not declare a service
ready and does not infer readiness from backend connectivity or an uploaded
artifact.

## Accepted contracts consulted

The Solution repository remains authoritative:

- `contracts/brake-health-runtime/brake-health-runtime-profile.v1.json`
  (1.1.0), including credential, readiness and asynchronous delivery boundaries;
- `contracts/brake-telemetry-window/brake-telemetry-window-profile.v1.json`
  (1.0.0), six signal paths, 250 ms freshness, capture and spool rules;
- `contracts/kuksa-current-demo-authorization/kuksa-auth-compat.v1.json`
  (1.7.0) and its closed request/response schemas;
- `contracts/brake-cloud-api/brake-cloud-api-profile.v1.json` (1.0.0) and
  `brake-cloud-ack.schema.json`, exact receipt identity and retry rules;
- `contracts/local-demo-hosting/local-demo-hosting-profile.v1.json`
  (D4-020), isolated guest-to-host transport, not production authentication.

Descriptions and schemas take precedence over examples. No new authority or
metadata-injection mechanism is established by this source change.

## Gates before product assembly or upload

1. **Authoritative runtime metadata binding is unresolved.** The executable
   needs the current Unit system UID, Unit role, service version/artifact
   digest and accepted VDP contract version/digest. The existing service
   package and named resources do not establish a complete authoritative
   delivery path for these inputs. An Aos instance identity alone does not
   supply the role and provenance digests. The integration design must specify
   ownership, delivery, validation and change handling; do not bake a current
   Test UID into a reusable package or derive digests from version labels.
2. **Public KUKSA TLS trust injection is unresolved.** Factory `.31` declares
   `kuksa` and a private `kuksa-auth-client` socket/token resource. This is not
   a service-visible public KUKSA server-trust mount. A Service needs an
   authoritative public trust input in addition to its private short-lived
   token. Do not use the Provider credential, copy a private key, disable TLS
   verification, use trust-on-first-use or silently widen named resources.
   Factory `.31` and its platform resources were not modified by this work.
3. **Linux ARM64 product build is not available or performed.** This host
   compile produces macOS C++ objects/tests, not Linux ARM64 executables. No
   qualified Linux ARM64 cross-toolchain, C++ gRPC/Protobuf runtime or generated
   KUKSA adapter was available in this lane. The eventual build must pin
   dependencies, produce real Linux ARM64 ELF executables, close shared-library
   and public-license requirements, and be invoked through Demo Control. No
   VM, Builder, Cloud, package-signing or upload operation was performed.

After these bindings are agreed, the remaining composition includes the
instance-secret bootstrap/renewal process boundary, KUKSA TLS Get/Subscribe
adapter, VDP compatibility/metadata checks, delivery scheduling and observable
readiness/overflow facts. Growing-window transport and v2/v3 composition also
remain explicit work; this library currently publishes a captured window only
when its terminal completion is durably available. Packaging must then request
the reviewed exact KUKSA permissions/resources, `instances.minInstances: 1`
and `offlineTTL: P7D`. Do not treat the old scaffold config as that product
configuration. Deployment still uses Group Subject assignment by `service_ids`,
not a caller-selected instance count or service version.

## Bounded local verification

The existing CMake/CTest path now includes `brake_health_runtime_contract`
alongside unchanged v1/v2 regression suites. The runtime suite has seven groups:
strict JSON, coherent frames, KAC envelopes, HTTP/retry bounds, durable ACK
delivery, restart/source-gap/conflict handling and private-file replacement.
All input records and ACKs in these tests are explicit isolated test data.
No live exchange, bootstrap lifecycle, quota qualification, ARM64 deployment or
E2E claim follows from their success.
