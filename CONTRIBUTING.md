<!-- SPDX-FileCopyrightText: 2026 maninblack -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Contributing

## Contribution License

Unless explicitly stated otherwise, an intentionally submitted contribution is
provided under the Apache License, Version 2.0, without additional terms or
conditions. Contributors retain copyright in their contributions; this project
does not initially require copyright assignment or a contributor license
agreement.

## Developer Certificate of Origin

Every accepted commit must certify the
[Developer Certificate of Origin 1.1](https://developercertificate.org/) with a
`Signed-off-by` trailer:

```text
Signed-off-by: Your Name <your-email@example.com>
```

Create the trailer with `git commit --signoff`. Sign off only when you have the
right to submit the work under the project license.

## Provenance

- Do not submit copied code without an applicable license and complete source
  attribution.
- Preserve all third-party notices and mark modifications where required.
- Do not relicense COVESA VSS-derived files from MPL-2.0 to Apache-2.0.
- Treat public repositories without an applicable license as reference-only.
- Do not submit credentials, private URLs, restricted Unreal Engine material,
  generated certificates, VM images, or account-specific logs.

## Scope

Application behavior, KUKSA/VSS contract consumption, tests, Aos service
packaging, resource limits, health reporting, and rollbackable releases are in
scope. Providers, KUKSA platform configuration, VM lifecycle, and Authorization
Adapter implementation belong outside this repository.
