<!-- SPDX-FileCopyrightText: 2026 maninblack -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Current-Test advisory demo control

Functional V3 emits the fixed Brake `Advisory.Readiness` actuator every five seconds from actual telemetry readiness. It polls only its existing Brake backend's fixed `/api/v1/brake/demo-control/poll` route. No inbound control listener is added.

`RESET_DEMO_SCENARIO` is bound to current Unit, native four-field service instance, installed release and persistent producer epoch, and expires after 60 seconds. The durable reset restores the existing preconditioned model and capture only. Identity, monotonic sequence, historical products and outbox are retained. No GOOD assessment or repair claim is emitted.

The service stops refreshing the old warning and writes typed CLEAR with the reset UUID as decision ID. Only a correlated Gateway CLEARED result produces a successful acknowledgement at `/api/v1/brake/demo-control/ack`. Duplicate/restart recovery cannot reset twice; a release/instance change rejects the pending command. Ordinary stop/crash does not synthesize CLEAR.

Source tests cover duplicate/restart, staged-write interruptions, retained history/sequence, failure/expiry and fresh/stale readiness. Live staging and clean Factory qualification are separate gates; local tests do not establish them.

