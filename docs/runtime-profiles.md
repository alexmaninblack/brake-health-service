<!-- SPDX-FileCopyrightText: 2026 maninblack -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Explicit Brake product runtime profiles

## Source boundary

The executable selects its immutable content at compilation using
`BHS_FUNCTIONAL_PROFILE=v1|v2|v3`. The default remains `v1`; every unknown
value fails CMake configuration. Neither the executable nor a package exporter
infers this selection from `serviceVersion`. Release numbers in the accepted
deployment metadata are carried unchanged in product messages and requests.
One Service identity can therefore publish successive releases of the same
functional profile.

| Profile | Live input | Product processing | Output |
| --- | --- | --- | --- |
| v1 | Six accepted base signals | Growing PRE/ACTIVE/POST capture | Durable window chunks and completion |
| v2 | Twelve accepted model signals | Fixed-point adapter, complete episode, existing v2 StateStore/model | Durable assessment and optional band-change event |
| v3 | The same twelve signals plus own read-only GatewayStatus | Exact v2 model-state reuse plus persistent advisory lifecycle | v2 output, own actuator request and correlated durable advisory fact |

The v2/v3 adapter never inserts zero-valued excluded v1 signals. Its source
types and normative units are validated against KUKSA metadata. Coherent
source timestamps, freshness, finite/range checks and one quantization precede
the model. No normal v1 window is emitted by v2/v3. The legacy v1 spool drains
independently and remains until exact durable acknowledgement.

## Entry point and existing external boundaries

The signed Aos service must start `/usr/bin/brake-health-bootstrap`, passing:

```text
--metadata-file /run/aosedge/platform/service-inputs/metadata.json
--ca-file /run/aosedge/platform/service-inputs/kuksa-ca.pem
```

Bootstrap alone consumes `AOS_SECRET` and exchanges it at the existing KAC
socket. It removes the secret before executing `/usr/bin/brake-health-service`.
The child uses the fixed owner-only JWT file and authenticated TLS KUKSA VAL
endpoint `Server:55555`. The metadata remains the closed seven-field schema
documented in `runtime-executable.md`; profile selection adds no metadata field.

The accepted packaging resources are `kuksa`, `kuksa-auth-client` and the
approved read-only `brake-runtime-inputs` mapping. Demo Control owns production
of authoritative metadata/public trust and the exact effective instance
identity. This source does not generate that identity, discover private trust,
reuse private keys, grant new paths or bypass TLS verification.

The only advisory write is an exact `FIELD_ACTUATOR_TARGET` for
`Vehicle.OEM.BrakeHealth.Advisory.Request`. GatewayStatus is read-only.
There is no Tire, motion or alternate-endpoint write. The v3 adapter reads the
current status before retrying or refreshing after reconnect, then subscribes
to that status independently of telemetry acquisition and backend transport.
Successful KUKSA Set means accepted transport, never applied advisory.

## Durable state

Existing model state stays in `model-state/v1`, relative to
`/storage/brake-health`; the existing v2 transaction journal and identity ledger
remain authoritative for exact-once model advancement. Advisory state occupies
the private `advisory-state/v1` subdirectory; durable facts occupy `v3/outbox`.
These are Service-owned persistent files, not additional host mounts or stores.

The advisory request, immutable deployment provenance, epoch and next sequence
are persisted before a KUKSA write. A lost/ambiguous write retries identical
bytes. Ordinary process/VM/container restart reuses the same epoch. A refresh
after 20 seconds has a new UUIDv5 request ID and monotonic sequence with a
30-second lease. An already active v2 condition creates one v3 activation using
its accepted last assessment, without a synthetic assessment or band event.
A subsequent same-band assessment does not invent another band transition.
Current monotonic model behavior never emits CLEAR; stop/crash lets the Gateway
expire the lease. A future CLEAR-producing migration is outside this runtime.

The private advisory journal records hashed before/after state and the optional
exact fact. Recovery verifies the closed state schemas, complete canonical
fact and its binding to the persisted request/status/provenance before any
write. It accepts only the recorded before/after state; conflicts remain
fail-closed, never silently reset. The recent correlation ledger is bounded;
unacknowledged facts have independent durable retention. v2 pairs and v3 facts
share the accepted 64-message/1-MiB budget. Overflow records non-enqueue and
does not stop local model/advisory progress or later fabricate a skipped fact.
409 or invalid acknowledgements retain and quarantine the exact message.

## Build/export contract

Only Demo Control performs artifact operations. Its existing Docker build
receives `--build-arg BHS_FUNCTIONAL_PROFILE=v1` (or `v2`/`v3`) in addition to
`SOURCE_REVISION` and `SOURCE_DATE_EPOCH`. The pinned `export` target still
builds the real KUKSA executable with `BHS_BUILD_KUKSA_RUNTIME=ON`, runs all four
CTest suites, and exports the two Linux ARM64 executables and dependency
closure. `product-build.json` adds `functionalProfile`; all existing paths and
fields remain intact. A controller catalog must distinguish source revision
and profile rather than overwrite one with another.

## Validation boundary

Native host tests cover the actual Product composition with deterministic
test-only telemetry: v2 model invocation, restart retention, v2-to-v3 active
condition activation, actual release provenance, ambiguous request retry,
exact Gateway correlation, 20-second refresh, durable backend ACK,
overflow/non-enqueue, conflict quarantine and v1 no-model separation. Recovery
tests cover journal-only, state-written and fact-written crash frontiers;
reopening and ACK do not resurrect delivered facts. Corrupt hashes, unknown
state schemas and conflicting records fail closed without replacing prior
state. Capture tests cover immediate missing/nonfinite input rejection,
POST-to-ACTIVE continuation and maximum-duration suppression. There are thirteen
application groups plus the three existing domain/runtime CTest suites.

Readiness logging uses the accepted combined modes, independently tracking
analytics, backend acknowledgement and v3 internal advisory capability.
A cached Gateway status is reconciled as evidence but does not establish fresh
chain readiness: that requires correlation to a request attempted in the
current session. A single command outcome is not treated as capability failure.

This is a source/host-tested checkpoint, not a successful deployment claim.
The integration owner reported a successful real Linux ARM64/gRPC v1 build of
`81e6afc1563239c8d75fb7183a50546cde7ddc58`; this later recovery/readiness
increment still requires a warm product build. Real KAC/Service-identity mount ownership,
actual backend transport, Gateway lease/application proof, requested RAM/CPU/
thread envelope, and clean restart/security evidence remain live gates owned
by Demo Control. No Docker, signing, publication,
VM or Cloud action was performed in the isolated source lane.
