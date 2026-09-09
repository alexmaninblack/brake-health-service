<!-- SPDX-FileCopyrightText: 2026 maninblack -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Third-Party Notices

The initial repository bootstrap does not copy, modify, generate from, or
bundle third-party source or binary material.

AosEdge, COVESA VSS and CARLA are referenced for architecture and compatibility;
their source is not distributed here. The optional product target now requires
the external pinned gRPC C++ and Protobuf libraries and generates C++ bindings
from the external Eclipse KUKSA 0.5.0 VAL schemas. The repository contains no
copied/generated upstream source, library, compiler or certificate.

The product dependency inventory is in `DEPENDENCIES.json`: gRPC is Apache-2.0
with transitive BSD-3-Clause/MPL-2.0 material; Protobuf is BSD-3-Clause; Abseil and
the KUKSA schemas are Apache-2.0. A final ARM64 artifact must include the complete
applicable notices for generated and linked material, including all transitive
dependencies. That artifact/link/license-closure gate is not complete yet.

Update this file before accepting any third-party file or publishing an Aos
service artifact that bundles a dependency. Preserve all applicable upstream
license, copyright, patent, trademark, attribution, and NOTICE requirements.
