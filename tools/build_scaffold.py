#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 maninblack
# SPDX-License-Identifier: Apache-2.0

"""Build an unsigned ARM64 Aos service staging directory."""

from __future__ import annotations

import argparse
import json
import shutil
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "build/aos-service-scaffold"


class ScaffoldError(RuntimeError):
    """Raised when a safe scaffold cannot be produced."""


def build_scaffold(output: Path) -> Path:
    output = output.resolve()
    if output.exists():
        raise ScaffoldError(f"output already exists: {output}")

    rootfs = output / "service/arm64"
    executable_target = rootfs / "usr/bin/vehicle-telemetry-service"
    config_target = rootfs / "etc/vehicle-telemetry-service/compatibility.json"
    license_target = rootfs / "usr/share/licenses/vehicle-telemetry-service"

    output.mkdir(parents=True)
    executable_target.parent.mkdir(parents=True)
    config_target.parent.mkdir(parents=True)
    license_target.mkdir(parents=True)

    shutil.copy2(ROOT / "packaging/aos/config.yaml", output / "config.yaml")
    shutil.copy2(ROOT / "src/usr/bin/vehicle-telemetry-service", executable_target)
    executable_target.chmod(0o755)
    shutil.copy2(ROOT / "config/compatibility.json", config_target)
    for name in ("LICENSE", "NOTICE", "THIRD_PARTY_NOTICES.md"):
        shutil.copy2(ROOT / name, license_target / name)

    compatibility = json.loads(config_target.read_text(encoding="utf-8"))
    if compatibility.get("target", {}).get("architecture") != "arm64":
        raise ScaffoldError("compatibility target must be arm64")
    if any(output.rglob("*.p12")) or any(output.rglob("*.pem")):
        raise ScaffoldError("credential material must not be copied into staging")
    return output


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()
    try:
        output = build_scaffold(args.output)
    except (OSError, ScaffoldError, json.JSONDecodeError) as exc:
        print(f"Scaffold build failed: {exc}", file=sys.stderr)
        return 1
    print(f"Unsigned ARM64 Aos service scaffold created: {output}")
    print("No certificate, signature, cloud identity, or telemetry behavior was added.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
