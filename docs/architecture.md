<!-- SPDX-FileCopyrightText: 2026 maninblack -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Brake Health Service Architecture Boundary

## Runtime Contract

The service is an independently deployable Aos application. It consumes a
compatible version of the vehicle telemetry contract through the
`kuksa.val.v1` API exposed by the Aos resource named `kuksa`. Future behavior
will derive brake-health observations and recommendations locally in the
vehicle without making Cloud connectivity part of the decision path.

```text
vehicle provider -> KUKSA Databroker -> Aos resource "kuksa" -> this service
```

The application does not know which vehicle provider produced a value. It has
no simulation endpoint, vehicle-bus adapter, platform configuration, VM
launcher, provisioning code, or dependency on platform repository source.

Brake Health is one independently deployable product with immutable v1, v2
and v3 release compositions. The current dependency-free C++17 domain layer is
the v1 foundation: complete-frame validation, every-third-frame retention,
PRE/ACTIVE/POST capture, closed canonical chunk/completion serialization and a
bounded POSIX spool. Injected clocks, source timestamps and UUIDs keep all
owned decisions deterministic and host-testable.

The R-3 scaffold requests read-only access to the KUKSA resource and no Aos
layers. A future implementation must declare every new runtime library as an
Aos layer or bundle it with complete license notices. It must not widen the
resource mode without a reviewed use case.

## Current Behavior

The C++17 v1 library implements domain behavior and durable-spool primitives,
but it is not yet wired into the Aos artifact. The packaged shell executable
is unchanged: it prints one English diagnostic message and exits. It does not
open a KUKSA or KAC connection, subscribe to telemetry, persist product data or
send data outside the vehicle. This keeps domain implementation evidence
separate from later adapter, packaging, integration and qualification claims.

## Configuration Ownership

- The service repository owns its compatibility range, resource request,
  quotas, command, environment, and package version.
- The platform owns the unmodified Eclipse KUKSA Databroker, Vehicle Data
  Provider, removable fixed-resource KAC helper, Aos IAM permission state and
  KUKSA trust configuration. KAC is not part of this repository or VDP.
- A later Service-owned bootstrap adapter will present only its per-instance
  `AOS_SECRET` over the private KAC socket. Resource `kuksa`, paths, modes,
  subject, audience, claims and lifetime are not caller-selected; KAC returns
  either the current IAM-derived short-lived JWT or a fail-closed rejection.
  No reusable KUKSA token belongs in the SOTA artifact.
- The integration repository selects exact compatible revisions and proves an
  end-to-end baseline.
