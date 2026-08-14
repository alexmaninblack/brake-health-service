<!-- SPDX-FileCopyrightText: 2026 maninblack -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Versioning, Resources, and Rollback

## Versioning

Repository and Aos service releases use semantic versioning. A release also
declares a vehicle telemetry contract range and KUKSA API name.

- Patch releases preserve behavior and contract compatibility.
- Minor releases add backward-compatible behavior or optional inputs.
- Major releases may require a new contract range, permissions, state format,
  or operator migration.

The R-3 `0.1.0` package is a diagnostic scaffold, not a production release.

## Resource Ownership

The package configuration in this repository is authoritative for service CPU,
RAM, storage, state, temporary storage, file descriptor, process, and network
limits. A release must justify increases. It cannot silently require a new Aos
resource, layer, device, port, or write permission.

## Rollback

Every accepted service version must remain independently installable. A new
version may not make irreversible external or local state changes before a
rollback path is qualified. State schema changes require explicit forward and
backward compatibility rules. The diagnostic scaffold has no persistent state.

## Product Language

All product UI, console output, logs, errors, package metadata, configuration,
and operator documentation are English. Conversation language and contributor
locale do not change this rule.
