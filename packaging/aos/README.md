<!-- SPDX-FileCopyrightText: 2026 maninblack -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Aos Service Packaging

`config.yaml` is a credential-free Aos signer schema-version-2 template for a
single ARM64 diagnostic service image. It references the conventional local
filename `aos-user-sp.p12`, but that certificate is neither included nor
tracked.

`tools/build_scaffold.py` creates the signer staging layout under an explicit
output directory:

```text
<output>/
├── config.yaml
└── service/arm64/
    ├── etc/vehicle-telemetry-service/compatibility.json
    ├── usr/bin/vehicle-telemetry-service
    └── usr/share/licenses/vehicle-telemetry-service/...
```

The staging directory is unsigned. Signing, service registration, upload, and
deployment begin only in the integration phase with a real Service Provider
credential and cloud service identity.

The root-level `config.yaml` layout follows the installed Aos signer
schema-version-2 SDK. Older Aos documentation that shows `meta/config.yaml`
describes the legacy package layout.

The `kuksa` resource mode is intentionally omitted. The current SDK declares
its default as read-only `r`, while its generated validation enum
inconsistently lists only `w` and `rw`. Omitting the field preserves the safe
default and validates; setting `rw` merely to satisfy the faulty enum would
unnecessarily widen access. KUKSA signal permissions remain a separate
Authorization Adapter concern.
