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

The R-3 scaffold requests read-only access to the KUKSA resource and no Aos
layers. A future implementation must declare every new runtime library as an
Aos layer or bundle it with complete license notices. It must not widen the
resource mode without a reviewed use case.

## Current Behavior

The packaged executable prints one English diagnostic message and exits. It
does not open a KUKSA connection, subscribe to telemetry, persist data, or send
data outside the vehicle. This makes the package structure testable without
claiming AOS-3 consumer behavior.

## Configuration Ownership

- The service repository owns its compatibility range, resource request,
  quotas, command, environment, and package version.
- The platform owns the KUKSA Databroker, resource mapping, provider, trust
  policy, and future Authorization Adapter.
- The integration repository selects exact compatible revisions and proves an
  end-to-end baseline.
