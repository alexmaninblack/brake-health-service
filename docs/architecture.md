<!-- SPDX-FileCopyrightText: 2026 maninblack -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Brake Health Service Architecture Boundary

## Runtime Contract

The service is an independently deployable Aos application. It consumes a
compatible version of the vehicle telemetry contract through the
`kuksa.val.v1` API exposed by the Aos resource named `kuksa`. V1 captures brake
windows, V2 derives local synthetic condition assessments and V3 adds typed
maintenance advisory, without Cloud connectivity in the local decision path.

```text
vehicle provider -> KUKSA Databroker -> Aos resource "kuksa" -> this service
```

The application does not know which vehicle provider produced a value. It has
no simulation endpoint, vehicle-bus adapter, platform configuration, VM
launcher, provisioning code, or dependency on platform repository source.

Brake Health is one independently deployable product with immutable v1, v2
and v3 release compositions. Its dependency-free C++17 domain layer now has
two source-complete foundations:

- v1 owns complete-frame validation, source-time 100-ms bucket retention
  (18 September cadence amendment; first actual valid sample, no interpolation),
  PRE/ACTIVE/POST capture, closed canonical chunk/completion serialization and
  a bounded POSIX spool; and
- v2 accepts an adapter-completed fixed-point episode, runs only
  `brake-condition-demo-v1`, emits the closed assessment and optional
  band-change event, and owns crash-safe model state plus a bounded derived
  outbox.

The v2 transaction order is immutable journal, atomic state replacement,
atomic assessment/event bundle publication and commit marker. Recovery accepts
only an exact before-state or after-state match. Every other generation,
digest, model identity or schema combination fails closed as
`NOT_READY_STATE`. Assessment plus optional event admission is atomic; outbox
overflow advances the accepted condition state and recent-source ledger once
but enqueues neither member of the pair. Exact durable ACK identity,
idempotency-key digest and content digest are required before local deletion.
The journal is removed only after rereading the committed state and exact
admitted bundle, or proving that an overflow transaction published no bundle.
Duplicate results use the verified identity retained in the outbox or state;
an internal canonical identity ledger binds every retained source-event entry
to its committed assessment identity and to the exact model-state generation
and digest. A canonical SHA-256 root covers the complete ordered binding list,
so changing any historical mapping is a fail-closed corruption even after its
outbox bytes have been acknowledged. Durable ACK removes the message bytes but
never that binding, so
every event still in the 64-entry ledger returns its exact historical identity
without deriving it from current deployment metadata.

Injected roots, source timestamps, processing time, Unit metadata and UUID
inputs keep all owned decisions deterministic and host-testable. UUIDv5/SHA-1
is used only for deterministic identifiers, never as authentication, signing
or integrity protection. The accepted v1 spool remains separate and byte-
unchanged while v2 initializes and evaluates.

The R-3 scaffold requests read-only access to the KUKSA resource and no Aos
layers. A future implementation must declare every new runtime library as an
Aos layer or bundle it with complete license notices. It must not widen the
resource mode without a reviewed use case.

## Current Behavior

The immutable compile-time V1/V2/V3 [runtime profiles](runtime-profiles.md)
compose real TLS KUKSA acquisition, local processing, persistent model/outboxes,
independent backend transport and V3 Gateway-correlated advisory. The Aos
entrypoint is the real credential bootstrap plus ARM64 product executable,
not the historical R-3 shell scaffold.

Public input schema 2 has five fields; package release and native instance
identity have separate authorities. KAC supplies private short-lived
credentials from current native IAM permissions. Services validate actual
local inputs and recover without Cloud; absence of input is not proof of an
incompatible installed VDP. Presenter maps the exact Cloud-installed profile.

[Reset Driver Advisory](advisory-demo-control.md) resets only the selected
model/advisory state through a command/application acknowledgement, preserving
history, queues and ordinary identity/sequence continuity. Stop/crash is not
a Reset. See the [integration matrix](../../aosedge-sdv-demo/docs/architecture/current-implementation.md)
for scoped .39 ignition/offline proof and remaining calibration/fault gates.

The original [runtime increment](runtime-increment.md) is historical source
evidence. Its original packaging/ARM64 gaps are not current product status.
The diagnostic scaffold is never a product fallback.

## Configuration Ownership

- The service repository owns its compatibility range, resource request,
  quotas, command, environment, and package version.
- The platform owns the unmodified Eclipse KUKSA Databroker, Vehicle Data
  Provider, removable fixed-resource KAC helper, Aos IAM permission state and
  KUKSA trust configuration. KAC is not part of this repository or VDP.
- The Service-owned bootstrap presents only its per-instance
  `AOS_SECRET` over the private KAC socket. Resource `kuksa`, paths, modes,
  subject, audience, claims and lifetime are not caller-selected; KAC returns
  either the current IAM-derived short-lived JWT or a fail-closed rejection.
  No reusable KUKSA token belongs in the SOTA artifact.
- The integration repository selects exact compatible revisions and proves an
  end-to-end baseline.
