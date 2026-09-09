<!-- SPDX-FileCopyrightText: 2026 maninblack -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Aos Service Packaging

For the actual Brake v1 executable candidate, use the Demo Control-owned
[Linux ARM64 product build](../../docs/product-build.md). Its output is
verified native binaries and public notices, not a signed deployment bundle.
The rest of this page describes the unchanged diagnostic scaffold and must
not be mistaken for the product publication path.

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
    ├── etc/brake-health-service/compatibility.json
    ├── usr/bin/brake-health-service
    └── usr/share/licenses/brake-health-service/...
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
unnecessarily widen access. The current packaged scaffold remains unchanged.
Future adapter packaging will use the separately owned fixed-resource KAC
boundary: the Service bootstrap presents only its per-instance `AOS_SECRET`;
resource `kuksa` is implicit, current Aos IAM permissions are authoritative,
and the Service cannot select paths, modes, subject, audience, lifetime or
claims. KAC returns only a short-lived path-scoped JWT or a fail-closed
rejection. No reusable token is packaged here.
