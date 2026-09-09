<!-- SPDX-FileCopyrightText: 2026 maninblack -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Brake v1 executable integration candidate

Status: bootstrap and host/domain contracts compiled and tested. The real C++
KUKSA adapter is implemented in source, but **has not yet been compiled or
executed with gRPC, installed on Linux ARM64, or qualified as deployable**.
The unchanged diagnostic scaffold must not be used for a Brake product upload.

## Process and transport boundary

- `/usr/bin/brake-health-bootstrap` is the intended Aos command. It consumes
  `AOS_SECRET`, removes that variable before exec of the analytics child, and
  uses only the accepted fixed KAC Unix socket. The child has no instance
  secret. KAC secrets and responses are never printed.
- KAC owns token issuance; bootstrap owns renewal and atomic mode-`0400`
  delivery to `/run/aosedge/secrets/kuksa/token.jwt`. The existing named
  resource must supply its private owner-only tmpfs directory. No fallback
  directory is created. `KUKSA_TOKEN_FILE` is a fixed child environment value.
- An independent bootstrap loop removes the token at signed expiry even when
  an eight-second renewal request is outstanding. BOOTTIME bounds prevent a
  backward wall-clock jump extending a lease. A terminal rejection removes
  the token and leaves analytics waiting, without a process-restart loop.
- `/usr/bin/brake-health-service` performs TLS-verified `kuksa.val.v1.Get`
  metadata inspection and `Subscribe` on exactly the six Brake v1 input paths.
  The endpoint is `Server:55555`, supplied by the existing `kuksa` resource's
  host mapping. There is no insecure channel or provider-credential fallback.
- KUKSA 0.5.0 authenticates the metadata header `authorization: Bearer <JWT>`;
  this was verified in upstream `databroker/src/grpc/server.rs:61`, not inferred
  from examples. Metadata must report sensor entries with the accepted
  float/uint8 types and `km/h`, `m/s^2`, or `percent` units.
- The subscription watcher cancels blocked RPCs on token removal/replacement,
  metadata change, trust-input change or shutdown. Freshness loss closes an
  active window rather than manufacturing source values. Renewed credentials
  result in a new Get and Subscribe.
- An independent delivery thread sends durable v1 messages to the fixed
  isolated Brake endpoint. Backend unavailability does not gate acquisition;
  retry preserves bytes, valid matching durable ACK permits deletion, and
  permanent conflict quarantines retained evidence.

## Growing-window delivery

The accepted D4-016.1/.2 and D4-017 messages are unchanged. The runtime seals
the PRE prefix separately from ACTIVE/POST: PRE chunks (including a short
last PRE chunk) become eligible as soon as the trigger checkpoint is durable.
Later complete ten-sample ACTIVE/POST chunks are eligible during capture;
their trailing partial chunk waits until full or terminal. The local
`ABORTED_RESTART` checkpoint is never sent while capture remains active.

This partition needs at most `ceil(PRE / 10) + ceil((ACTIVE + POST) / 10)`
chunks: the accepted bounds of 30 PRE and 120 ACTIVE/POST samples retain the
existing maximum of 15. It does not require warming a full PRE ring before
the first trigger. The completed-message serializer and its golden vectors
remain unchanged; the runtime uses the additional growing-message serializer.
The backend's existing cumulative `firstSampleIndex` validation accepts these
boundaries; no backend API or contract version change is required.

An ACK during capture never deletes the event. Sealed bytes cannot change
while in flight, terminal publication preserves acknowledged chunks, and the
single terminal completion commits to the exact transmitted digest sequence.
Recovery preserves chunk ACKs and closes intact captures as `ABORTED_RESTART`.
A torn multi-file checkpoint is quarantined when individually valid chunk
files and completion disagree. HTTP conflict similarly freezes retained
bytes without crashing subsequent acquisition/stop. A capacity-rejected event
cannot be partly re-admitted merely because an older event drains later.

These are host-tested producer/queue behaviors, not evidence of a running
backend, ARM64 deployment, gRPC data acquisition or observed dashboard growth.

No service-managed cgroups, resource claims, VM manipulation, Cloud calls,
simulator reads, raw telemetry logging or separate log archive are introduced.

## Explicit application inputs, not a new platform interface

Both executables require exactly these options, with absolute file paths:

```text
--metadata-file <authoritatively supplied service metadata JSON>
--ca-file <authoritatively supplied public KUKSA TLS trust PEM>
```

This is an approved **Service-owned application configuration boundary**.
It does not establish a guest mount, a metadata producer, a default UID, or a
deployment mechanism. Missing/invalid inputs fail closed; no identity is
derived from a version label, Subject label, hostname, or current operator.

The closed metadata object has seven required fields:

| Field | Validation and ownership requirement |
| --- | --- |
| `schemaVersion` | Exactly integer `1` |
| `unitSystemUid` | Nonempty bounded identifier; must originate from the current Unit's authoritative system identity |
| `unitRole` | Exactly `validation` or `production`; supplied by the agreed role binding |
| `serviceVersion` | Bounded semantic version, bound to the active service instance |
| `serviceArtifactSha256` | Lowercase 64-hex digest, with accepted artifact-digest meaning; not an invented binary digest |
| `vdpContractVersion` | Bounded semantic version from the accepted active VDP contract |
| `vdpContractSha256` | Lowercase 64-hex digest of the accepted active contract |

Additional properties, missing fields, malformed JSON, duplicate keys, invalid
role/version/digests and over-size input are rejected. The executable permits
VDP provenance refresh but refuses changes to Unit UID/role or service
version/artifact identity during that process lifetime. Before accepting VDP
metadata changes it closes the old active window with its old provenance;
queued messages are never relabelled.

## Precise live binding gate

The examined Factory `.31` uses platform commit
`0bed8b3769b09fbe685ed599ca8d10e6594fbe53` and immutable image SHA-256
`a9019f4adfe70499bde339c8e9d95eb8568736b73dc218f6c0e390fbcd28ddf4`.

| Available source/mechanism | What it proves / what it does not provide |
| --- | --- |
| Native IAM `GetSystemInfo.system_id` and official SDK `system_uid` | Authoritative Unit system identity exists; it is not one of the standard Service environment fields |
| Standard SM environment `AOS_ITEM_ID`, `AOS_SUBJECT_ID`, `AOS_INSTANCE_INDEX`, `AOS_INSTANCE_ID`, `AOS_SECRET` | Identifies the service instance and its IAM authorization secret; does not supply all six message provenance values |
| Existing Aos package environment overrides | A supported carrier, not by itself an authoritative source or agreed change lifecycle; baking the current UID into reusable packages is not acceptable |
| `.31` named resource `kuksa` | Adds host `Server` -> `10.0.0.100`; no filesystem trust input |
| `.31` named resource `kuksa-auth-client` | Supplies KAC socket and per-instance token tmpfs; does not expose the KUKSA public TLS certificate |
| `/var/lib/aos-kuksa-tls/server.pem` | Per-vehicle public, self-signed KUKSA TLS leaf; generated SAN is `IP:127.0.0.1,DNS:Server` (`tls_prepare.cpp:173`) |
| Provider systemd `LoadCredential=kuksa-ca:.../server.pem` | Demonstrates a working public-trust source for the platform Provider, not an existing Service-visible mount |

If no already-supported service-visible carrier closes these inputs, the
smallest proposed integration delta to review is **read-only public leaf
delivery plus read-only authoritative metadata delivery/change ownership**.
Only the public leaf is needed, never its sibling private key or Provider JWT.
The minimal read-only metadata/public-trust interface and a transient Test
proof without rebuilding `.31` are now authorized. This Service repository
does not implement the guest resource; the integration owner implements and
verifies the exact binding.
Native API availability does not justify giving a Service new IAM/network
access; the exact supported delivery mechanism still needs to be agreed and
tested. A Factory rebuild is not assumed necessary or authorized by this work.

## Exact build inputs for Demo Control

Product preparation must invoke the following through the Demo Control build
implementation, inside the selected Linux ARM64 toolchain environment:

```text
cmake -S <service-source> -B <owned-build-dir>
  -DBHS_BUILD_KUKSA_RUNTIME=ON -DBUILD_TESTING=ON
  -DBHS_KUKSA_SOURCE_ROOT=<pinned-kuksa-checkout>
  -DCMAKE_PREFIX_PATH=<pinned-grpc-protobuf-install-prefix>
  -DCMAKE_INSTALL_PREFIX=/usr
cmake --build <owned-build-dir>
ctest --test-dir <owned-build-dir> --output-on-failure
DESTDIR=<owned-rootfs> cmake --install <owned-build-dir>
```

These are build-tool inputs, not new operator workflow wrappers. CMake does
not download dependencies. The product option fails configuration if required
dependencies are absent; its default OFF is explicitly labelled host/domain
only and must never be used to assert product success.

The subsequent [product Docker recipe and export contract](product-build.md)
provides the pinned Linux ARM64 build path for Demo Control. Docker was not
executed by the source implementation lane; the first real build and
transient-Test qualification remain owned by integration.

Pinned inputs match the recorded Factory C++ versions:

| Input | Exact revision |
| --- | --- |
| gRPC / `grpc_cpp_plugin` 1.60.1 | `e5ae3b6b44bf3b64d24bfb4b4f82556239b986db` |
| Protobuf / `protoc` 4.25.8 (CMake package 25.8.0) | `a4cbdd3ed0042e8f9b9c30e8b0634096d9532809` |
| Abseil 20240116.3 | `54fac219c4ef0bc379dfffb0b8098725d77ac81b` |
| KUKSA databroker 0.5.0 VAL schemas | `30e5c13abc496d0b39aaa6c25acebb088b9902e3` |

KUKSA schema contents are also SHA-256 checked in CMake. External generated
headers are produced in the build directory and are not committed. Linux
ARM64 ELF architecture, libc/loader compatibility, complete link closure,
transitive license notices and actual gRPC compilation are remaining build
evidence, not implied by this input list. Do not substitute incompatible
distribution gRPC libraries merely because their package names match.

## Verification scope and remaining product work

Four host CTest targets pass: existing v1/v2 domains, sixteen runtime protocol/
durability groups, and five application input/token/clock/provenance groups.
`brake-health-bootstrap` compiles with warnings-as-errors. Tests use isolated
fixtures only; no runtime fixture records are installed or sent to a live
backend. There is no claim of bootstrap/child/KAC socket E2E or TLS gRPC fixture
E2E yet, because the product toolchain is unavailable in this lane.

Before SOTA: close the live binding above, compile/run the actual adapter,
qualify renewal/rejection/reconnect with an isolated KAC+TLS fixture, assemble
real ARM64 executables and library/license closure through Demo Control, then
set the accepted package permissions/resources, `minInstances: 1` and
`offlineTTL: P7D`. Current quota requests remain unqualified, especially gRPC
thread and memory use. Growing-window publication is implemented and locally
tested; real KUKSA-to-backend/dashboards verification is still outstanding.
Capacity/quarantine readiness and aggregated operational-fact reporting need
separate executable qualification; local retention tests do not establish
those operator-visible states. v2/v3 runtime and advisory composition are
separate subsequent work; the [v2/v3 wiring audit](runtime-v2-v3-audit.md)
records exact missing runtime calls and the release/profile schema conflict.
