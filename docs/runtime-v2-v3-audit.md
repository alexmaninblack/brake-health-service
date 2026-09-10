<!-- SPDX-FileCopyrightText: 2026 maninblack -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Brake v2/v3 runtime wiring audit

Historical checkpoint: `b6ba7a257271070b0a668b9e726d45e44b677e29`.
The findings below describe that checkpoint, not the current implementation.
They were subsequently addressed by the explicit runtime profile work in
[runtime-profiles.md](runtime-profiles.md); live qualification remains separate.

Scope: accepted Studio P5/P6, D4-016.1 through D4-016.5 and D4-017. This is a
source audit plus bounded host corrections, not a deployable-v2/v3 or E2E claim.
The preceding growing-v1 checkpoint remains independently available at
`4434082b25dfaf7e53462c7ff22986cb7b500659`.

## State at the audited checkpoint

| Boundary | Source evidence | Required next wiring |
| --- | --- | --- |
| Functional profile selection | `grpc_main.cpp` constructs the v1 `Runtime`; the build/export and seven-field application metadata contain no explicit content-profile selector | Freeze v1/v2/v3 as immutable build/candidate content, independently of the monotonically increasing release. Do not select by release major version. The shared build/export carrier requires integration-owner agreement. |
| v2 acquisition | `runtime.hpp::paths` and `grpc_main.cpp::subscribe` handle six v1 leaves only; `v2::CompletedEpisode` expects twelve fixed-point model signals | A coherent twelve-path subscriber and one-time normative unit conversion/quantization, retaining original source time/quality. Do not synthesize zeros for excluded lateral/vertical/accelerator inputs merely to reuse the six-field v1 type. |
| v2 model execution | `SyntheticModel` and `StateStore::process` are implemented and host-tested; the executable never calls them | Compose the accepted episode boundary, injected deployment/processing metadata and the existing crash-safe state transaction. Start v2 analytics without waiting for the retained legacy v1 spool. |
| v2 derived backend delivery | The shared ACK matcher now covers all five D4-017 message kinds; `derived_delivery.cpp` bridges actual `StateStore` entries to exact ACK acceptance and conflict retention | Invoke the adapter from the independent runtime delivery thread, outside the model/store lock during HTTP. The bridge alone does not start a sender. |
| v3 advisory | No v3 producer, request persistence, KUKSA target write, Gateway Status subscription or advisory-fact queue is present in this committed worktree | Compose D4-016.4 and typed QM request/status through the accepted own endpoint. Preserve epoch/sequence before write; reconcile status on restart, refresh at 20 seconds, use the 30-second lease, and never treat transport success as Gateway application. |
| State continuity | `StateStore` requires an epoch and rejects a mismatching persisted epoch; the application does not initialize/recover that owner | Preserve the persisted epoch and exact model state for ordinary restart and v2-to-v3 update. First creation and explicit producer replacement must retain the accepted lifecycle meanings. |
| Qualification | Host domain/queue tests only | Actual pinned gRPC/ARM64 build, corresponding VDP capabilities, approved KAC/TLS/metadata/resource bindings, backend records, advisory status and quota measurements remain separate gates. |

The separate historical v3 worktree contains untracked draft files, not an
immutable qualified candidate. This task neither changes nor imports those
files. Merely linking the v2 library into the shared ACK adapter does not wire
v2 behavior into the application entrypoint.

## Contract-defined corrections in this increment

1. `SyntheticModel::evaluate` accepts POST-to-ACTIVE retrigger in the same
   event, as D4-016.1 requires. PRE remains a prefix and POST cannot precede
   the first ACTIVE. The existing model arithmetic and golden result are
   unchanged. A regression verifies one assessment/state advance and duplicate
   suppression after a retriggered episode.
2. `matches_ack` uses the exact D4-017 key for window chunk, completion,
   assessment, event and advisory fact. Advisory facts additionally bind the
   Gateway state: an APPLIED receipt cannot acknowledge EXPIRED. Unit and
   content digest remain exact, and HTTP 202 is not durable acceptance.
3. The derived-delivery adapter revalidates retained bytes before applying an
   ACK. Retryable/ambiguous responses preserve the bytes. A permanent failure
   or mismatched success quarantines the retained atomic bundle, excludes it
   from automatic retry and does not delete its model state or peer message.
   A delivery conflict does not block future otherwise valid local analysis.

Host tests exercise real `StateStore` output using explicitly isolated model
fixtures: retry, exact new/duplicate receipt, partial-pair acknowledgement,
restart, conflict retention and continued analytics. No fixture input or
product record becomes a deployed runtime source.

## Cross-repository release/profile conflict — unresolved

The accepted Studio plan requires independently bound content profiles and
monotonically increasing service releases, including later repeats of v1/v2/v3.
Current executable schemas and validators still freeze historical release
numbers:

- `brake-health-assessment.schema.json` and `brake-health-event.schema.json`
  allow `serviceVersion` only `2.0.0` or `3.0.0`;
- `brake-advisory-fact.schema.json` requires exactly `3.0.0`;
- this repository's `src/v2/messages.cpp::validate_metadata` enforces the
  same two release numbers; and
- the Brake backend's `brake-data-contract.ts` repeats those restrictions in
  `validateAssessment`, `validateEvent` and `validateAdvisory`.

A later release carrying the v2 profile would therefore be rejected by both
producer and backend even if its runtime were correct. This requires a
coordinated accepted-schema/producer/backend correction and an immutable
profile-to-release binding. It cannot be fixed by reporting a false old
`serviceVersion`, guessing functionality from SemVer or modifying a private
copy of the shared schema. This source increment changes none of those gates.

## Verified and excluded

Four CTest targets pass, including 16 runtime groups and 12 v2 domain groups;
the host bootstrap and affected libraries compile with warnings as errors.
No Docker build, live process, VM, Cloud operation, publication or push is
performed. The functional-profile carrier, executable v2/v3 composition,
release-schema correction and live qualification are not completed here.
