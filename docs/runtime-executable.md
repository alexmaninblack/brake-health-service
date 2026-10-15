<!-- SPDX-FileCopyrightText: 2026 maninblack -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Brake executable and deployment boundary

The product is the real Linux ARM64 credential bootstrap and KUKSA analytics
executable. Immutable [runtime profiles](runtime-profiles.md) select V1 capture,
V2 condition analysis or V3 advisory independently of the allocated release
number. A host build or diagnostic scaffold is not a deployable substitute.

Use the [product build contract](product-build.md) for exact toolchain,
dependency, output and export requirements. The integration
[current baseline](https://github.com/alexmaninblack/aosedge-sdv-demo/blob/main/docs/qualification/current-baseline.md)
owns candidate-specific deployment results and remaining gates; earlier
source-increment diaries are not current installation instructions.

## Process, identity and credentials

The Aos entrypoint is `/usr/bin/brake-health-bootstrap`, with absolute inputs:

```text
--metadata-file /run/aosedge/platform/service-inputs/metadata.json
--ca-file /run/aosedge/platform/service-inputs/kuksa-ca.pem
```

Demo Control and the platform supply authoritative public metadata and public
KUKSA trust through the accepted read-only resources. This service does not
invent Unit identity, generate trust, access a Provider private key or grant
itself resources.

Bootstrap consumes native `AOS_SECRET`, exchanges it at the fixed KAC Unix
socket and removes it before executing `/usr/bin/brake-health-service`.
Only bootstrap sees that instance secret. KAC issues IAM-derived short-lived
credentials; the analytics child receives its private token path.

Each bootstrap creates a fresh owner-only session beneath the per-container
tmpfs. The token is delivered atomically with mode 0400; unsafe ownership,
symlinks/hardlinks and excessive orphan sessions fail closed. Normal exit
removes only its own session; container destruction clears the tmpfs.
An independent expiry loop removes the token at signed expiry, including
during an outstanding renewal. BOOTTIME prevents a backward wall clock from
extending a lease. Terminal rejection waits for valid authority without a
process-restart storm. No Provider-token or insecure-channel fallback exists.

## Acquisition and freshness

The child uses TLS-verified `kuksa.val.v1.Get` metadata and `Subscribe` at
`Server:55555`, mapped by the platform's `kuksa` resource. Authorization is
the Bearer JWT metadata header. Subscriptions explicitly select `FIELD_VALUE`;
a Get view is not assumed to select subscription fields.

V1 consumes six base signals; V2/V3 consume twelve model signals. KUKSA
metadata must match their types and units. V1 retains the first complete valid
frame per 100-ms source-time bucket. V1/V2/V3 use the accepted 5000-ms
source-age/idle bounds; V2/V3 additionally bound cross-signal skew to 100 ms.
Missing, future, mixed, duplicate, reordered or physically invalid data is
rejected, not filled with zero or a refreshed receipt timestamp.

The input watcher is separate from longer processing and backend delivery.
Credential replacement/removal cancels blocked RPCs; renewal creates a fresh
authenticated Get/Subscribe without fabricating data or resetting model state.
VDP compatibility, local input readiness, backend delivery and advisory
application are independent observations.

## Public inputs and provenance

Public metadata has exactly five fields: `schemaVersion: 2`,
`unitSystemUid`, `unitRole`, `vdpContractVersion` and `vdpContractSha256`.
The immutable package file `/usr/share/aosedge/service-release.json` owns
`serviceVersion`. Native Aos environment owns `AOS_ITEM_ID`,
`AOS_SUBJECT_ID`, `AOS_INSTANCE_INDEX` and `AOS_INSTANCE_ID`.
Public JSON cannot override that instance identity.

All five product message kinds use wire schema 2 / contract 2.0.0 and closed
`serviceInstance` provenance. Neither service nor model OCI digest is emitted.
Functional profile, package release, model configuration digest and VDP
contract digest have separate meanings. Legacy queued bytes retain their
original schema, identity, checksums and receipts; legacy decoders do not
authorize a seven-field public file to start a new process.

## Durable processing and delivery

V1 seals the PRE prefix independently from growing ACTIVE/POST chunks.
Only durably sealed chunks are eligible for delivery; partial trailing data
waits for completion. ACK during capture does not delete the event. Restart
preserves acknowledgements and closes intact interrupted capture with its
original provenance. Conflicting/torn content is quarantined, not silently
rewritten or admitted again after capacity changes.

V2/V3 retain the crash-safe model journal, identity ledger and bounded outbox.
V3 adds its own persisted request/sequence/correlation state and only the
accepted Brake advisory write. The Gateway's correlated status, not KUKSA Set
success or a backend ACK, establishes advisory application.
[Reset Driver Advisory](advisory-demo-control.md) is a separate bounded
command/application protocol; it preserves history and delivery records.

An independent sender posts to the fixed isolated demo endpoint
`http://10.0.0.1:18091/api/v1/brake/messages`. Retry preserves exact bytes;
only matching durable acknowledgement permits deletion. Permanent conflict
retains/quarantines evidence without stopping later local analysis.
Backend loss does not gate acquisition or create vehicle-motion authority.

## Build, installation and acceptance

Demo Control resolves committed sources and runs the pinned Linux ARM64
[build/export recipe](product-build.md), including its mandatory tests and
ELF/dependency/license checks. The exported executables are assembled with
the accepted permissions, named resources and immutable release metadata
before signing/publication. Build output alone is unsigned and not live-qualified.

The selected integrated runtime has scoped real KUKSA, products, advisory,
update, reset, ignition and offline evidence. Full model/stimulus calibration,
quota stress, all security/transport negatives and nonempty-outbox power loss
remain separate obligations. Do not replay an old not-deployed status as a
current blocker, or use a sampled pass to close those broader obligations.
