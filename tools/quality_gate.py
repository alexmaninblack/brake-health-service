#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 maninblack
# SPDX-License-Identifier: Apache-2.0

"""Run dependency, credential, binary, language, and SPDX policy checks."""

from __future__ import annotations

import json
import re
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EXCLUDED_FROM_HEADER = {"LICENSE", "NOTICE", "LICENSES/Apache-2.0.txt"}
BINARY_SUFFIXES = {
    ".7z", ".bin", ".cer", ".crt", ".der", ".dmg", ".img", ".iso",
    ".jar", ".jks", ".key", ".ova", ".ovf", ".p12", ".pfx", ".pem",
    ".qcow2", ".tar", ".war", ".zip",
}
ALLOWED_DEPENDENCY_LICENSES = {"Apache-2.0", "BSD-2-Clause", "BSD-3-Clause", "GPL-3.0-or-later", "MIT", "MPL-2.0"}


def repository_files() -> list[Path]:
    result = subprocess.run(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard"],
        cwd=ROOT,
        check=True,
        text=True,
        capture_output=True,
    )
    return [ROOT / line for line in result.stdout.splitlines() if line]


def check_spdx(files: list[Path]) -> list[str]:
    errors: list[str] = []
    copyright_tag = "SPDX-FileCopyright" + "Text: 2026 maninblack"
    license_tag = "SPDX-License-" + "Identifier: Apache-2.0"
    for path in files:
        relative = path.relative_to(ROOT).as_posix()
        if relative in EXCLUDED_FROM_HEADER:
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        if copyright_tag not in text:
            errors.append(f"{relative}: missing approved SPDX copyright")
        if license_tag not in text:
            errors.append(f"{relative}: missing Apache-2.0 SPDX identifier")
    return errors


def check_binaries_and_credentials(files: list[Path]) -> list[str]:
    errors: list[str] = []
    private_key_marker = "-----BEGIN " + "PRIVATE KEY-----"
    patterns = (
        re.compile(r"(?i)(?:access|api|auth|client|refresh)[_-]?token\s*[:=]\s*['\"][^'\"]{12,}"),
        re.compile(r"AKIA[0-9A-Z]{16}"),
        re.compile(re.escape(private_key_marker)),
    )
    for path in files:
        relative = path.relative_to(ROOT).as_posix()
        if path.suffix.lower() in BINARY_SUFFIXES:
            errors.append(f"{relative}: prohibited binary or credential file type")
            continue
        data = path.read_bytes()
        if b"\x00" in data:
            errors.append(f"{relative}: binary content is not permitted in the scaffold")
            continue
        if relative == "tools/quality_gate.py":
            continue
        text = data.decode("utf-8", errors="replace")
        if any(pattern.search(text) for pattern in patterns):
            errors.append(f"{relative}: possible committed credential")
    return errors


def check_dependencies() -> list[str]:
    errors: list[str] = []
    inventory = json.loads((ROOT / "DEPENDENCIES.json").read_text(encoding="utf-8"))
    for scope in ("runtime", "build", "aosLayers", "ci"):
        dependencies = inventory.get(scope)
        if not isinstance(dependencies, list):
            errors.append(f"DEPENDENCIES.json: {scope} must be an array")
            continue
        for dependency in dependencies:
            name = dependency.get("name", "<unnamed>")
            if dependency.get("license") not in ALLOWED_DEPENDENCY_LICENSES:
                errors.append(f"DEPENDENCIES.json: {name} has an unknown license")
            if not re.fullmatch(r"[0-9a-f]{40}", dependency.get("revision", "")):
                errors.append(f"DEPENDENCIES.json: {name} is not pinned to a commit")
    return errors


def check_product_language() -> list[str]:
    errors: list[str] = []
    cyrillic = re.compile(r"[\u0400-\u04ff]")
    for root_name in ("src", "config", "packaging"):
        for path in (ROOT / root_name).rglob("*"):
            if path.is_file():
                text = path.read_text(encoding="utf-8")
                if cyrillic.search(text):
                    errors.append(f"{path.relative_to(ROOT)}: product content must be English")
    return errors


def main() -> int:
    files = repository_files()
    errors = check_spdx(files)
    errors.extend(check_binaries_and_credentials(files))
    errors.extend(check_dependencies())
    errors.extend(check_product_language())
    if errors:
        print("Repository quality gate failed:", file=sys.stderr)
        for error in errors:
            print(f"- {error}", file=sys.stderr)
        return 1
    print(f"Repository quality gate passed for {len(files)} files.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
