#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 maninblack
# SPDX-License-Identifier: Apache-2.0

"""Build an unsigned ARM64 Aos service staging directory."""

from __future__ import annotations

import argparse
import json
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "build/aos-service-scaffold"


class ScaffoldError(RuntimeError):
    """Raised when a safe scaffold cannot be produced."""


PRODUCT_BINARIES = ("brake-health-bootstrap", "brake-health-service")
PRODUCT_KIND = "brake-health-linux-arm64-product"
ALLOWED_NEEDED = frozenset({"libc.so.6", "libm.so.6", "ld-linux-aarch64.so.1"})
DEPENDENCY_REVISIONS = {
    "abseil": "54fac219c4ef0bc379dfffb0b8098725d77ac81b",
    "protobuf": "a4cbdd3ed0042e8f9b9c30e8b0634096d9532809",
    "grpc": "e5ae3b6b44bf3b64d24bfb4b4f82556239b986db",
    "kuksa": "30e5c13abc496d0b39aaa6c25acebb088b9902e3",
    "grpc/third_party/cares/cares": "6360e96b5cf8e5980c887ce58ef727e53d77243a",
    "grpc/third_party/re2": "0c5616df9c0aaa44c9440d87422012423d91c7d1",
    "grpc/third_party/zlib": "04f42ceca40f73e2978b50e93806c2a18c1281fc",
    "grpc/third_party/envoy-api": "9d6ffa70677c4dbf23f6ed569676206c4e2edff4",
    "grpc/third_party/googleapis": "2f9af297c84c55c8b871ba4495e01ade42476c92",
    "grpc/third_party/opencensus-proto": "4aa53e15cbf1a47bc9087e6cfdca214c1eea4e89",
    "grpc/third_party/opentelemetry": "60fa8754d890b5c55949a8c68dcfd7ab5c2395df",
    "grpc/third_party/protoc-gen-validate": "fab737efbb4b4d03e7c771393708f75594b121e4",
    "grpc/third_party/xds": "e9ce68804cb4e64cab5a52e3c8baf840d4ff87b7",
}
OPENSSL_ARCHIVE_SHA256 = "89681a9ddaa9ed7cf25ea8ef61338db805200bae47d00510490623547380c148"


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _readelf(path: Path, option: str) -> str:
    return subprocess.run(
        ["readelf", option, "--wide", str(path)], check=True, text=True,
        capture_output=True, timeout=15, env={**os.environ, "LC_ALL": "C"},
    ).stdout


def inspect_product_elf(path: Path) -> dict[str, object]:
    """Validate a real linked artifact; a diagnostic shell is never accepted."""
    if path.is_symlink() or not path.is_file() or not os.access(path, os.X_OK):
        raise ScaffoldError("product executable must be an executable regular file")
    with path.open("rb") as stream:
        header = stream.read(64)
    if len(header) != 64 or header[:7] != b"\x7fELF\x02\x01\x01":
        raise ScaffoldError("product executable must be 64-bit little-endian ELF")
    file_type, machine, version = struct.unpack_from("<HHI", header, 16)
    if file_type not in (2, 3) or machine != 183 or version != 1:
        raise ScaffoldError("product executable must be linked Linux AArch64 ELF")
    program_headers = _readelf(path, "--program-headers")
    interpreters = re.findall(r"Requesting program interpreter: ([^\]]+)", program_headers)
    if interpreters != ["/lib/ld-linux-aarch64.so.1"]:
        raise ScaffoldError("unexpected product ELF interpreter")
    dynamic = _readelf(path, "--dynamic")
    needed = sorted(set(re.findall(r"\(NEEDED\).*Shared library: \[([^\]]+)\]", dynamic)))
    if "libc.so.6" not in needed or not set(needed).issubset(ALLOWED_NEEDED):
        raise ScaffoldError("product contains an unclosed dynamic dependency")
    if re.search(r"\((?:RPATH|RUNPATH)\)", dynamic):
        raise ScaffoldError("product must not carry a build-machine runtime search path")
    version_info = _readelf(path, "--version-info")
    versions = sorted(set(re.findall(r"\bGLIBC_(\d+(?:\.\d+)+)\b", version_info)),
                      key=lambda text: tuple(map(int, text.split("."))))
    if not versions or any(tuple(map(int, value.split("."))) > (2, 36) for value in versions):
        raise ScaffoldError("product requires an unexpected glibc symbol version")
    return {
        "path": f"rootfs/usr/bin/{path.name}", "sha256": _sha256(path),
        "size": path.stat().st_size, "interpreter": interpreters[0],
        "needed": needed, "glibcVersions": versions,
    }


def inspect_test_report(path: Path) -> dict[str, object]:
    """Require the actual successful CTest report from the product build."""
    if path.is_symlink() or not path.is_file() or path.stat().st_size > 1024 * 1024:
        raise ScaffoldError("bounded CTest report is required")
    tree = ET.parse(path).getroot()
    suites = [tree] if tree.tag == "testsuite" else list(tree.findall("testsuite"))
    cases = [case for suite in suites for case in suite.findall("testcase")]
    expected = {
        "brake_demo_mock_isolation",
        "native_service_inputs", "brake_private_token_session",
        "brake_health_v1_contract", "brake_health_v2_contract",
        "brake_health_runtime_contract", "brake_health_application_contract",
    }
    if {case.get("name") for case in cases} != expected or len(cases) != len(expected):
        raise ScaffoldError("product CTest report does not contain every required suite")
    if any(int(suite.get("failures", "0")) or int(suite.get("errors", "0")) for suite in suites):
        raise ScaffoldError("product CTest suite failed")
    if any(case.find("failure") is not None or case.find("error") is not None or
           case.find("skipped") is not None for case in cases):
        raise ScaffoldError("product CTest case failed or was skipped")
    return {"ctest": "passed", "count": len(cases), "report": "evidence/ctest-results.xml",
            "reportSha256": _sha256(path)}


def export_runtime(runtime_root: Path, output: Path, source_revision: str,
                   source_date_epoch: int, dependency_source_root: Path, test_report: Path,
                   functional_profile: str = "v1") -> Path:
    """Internal packaging-library export used by Docker, invoked by Demo Control."""
    if not re.fullmatch(r"[0-9a-f]{40}", source_revision) or source_date_epoch <= 0:
        raise ScaffoldError("exact source revision and commit timestamp are required")
    if functional_profile not in ("v1", "v2", "v3"):
        raise ScaffoldError("explicit product content profile must be v1, v2 or v3")
    runtime_root, output, dependency_source_root = runtime_root.resolve(), output.resolve(), dependency_source_root.resolve()
    if output.exists() or output == runtime_root or runtime_root in output.parents:
        raise ScaffoldError("export must use a new output directory outside the input rootfs")
    binaries = [inspect_product_elf(runtime_root / "usr/bin" / name) for name in PRODUCT_BINARIES]
    tests = inspect_test_report(test_report)
    dependencies = []
    for name, expected in DEPENDENCY_REVISIONS.items():
        path = dependency_source_root / name
        revision = subprocess.run(["git", "-C", str(path), "rev-parse", "HEAD"],
                                  check=True, text=True, capture_output=True, timeout=15).stdout.strip()
        if revision != expected:
            raise ScaffoldError("dependency source revision mismatch")
        dependencies.append({"name": name, "revision": revision})
    if _sha256(dependency_source_root / "openssl.tar.gz") != OPENSSL_ARCHIVE_SHA256:
        raise ScaffoldError("OpenSSL archive digest mismatch")
    dependencies.append({"name": "openssl", "version": "3.2.6", "archiveSha256": OPENSSL_ARCHIVE_SHA256})

    output.mkdir(parents=True)
    binary_output = output / "rootfs/usr/bin"
    binary_output.mkdir(parents=True)
    for name in PRODUCT_BINARIES:
        shutil.copy2(runtime_root / "usr/bin" / name, binary_output / name)
        (binary_output / name).chmod(0o755)
    notices = output / "rootfs/usr/share/licenses/brake-health-service"
    notices.mkdir(parents=True)
    for name in ("LICENSE", "NOTICE", "THIRD_PARTY_NOTICES.md", "DEPENDENCIES.json"):
        shutil.copy2(ROOT / name, notices / name)
    # Preserve upstream notices by relative path; no certificates or test
    # payloads are selected. Pin/source URLs are retained in dependency metadata.
    for name in ("abseil", "protobuf", "grpc", "kuksa", "openssl-3.2.6"):
        source = dependency_source_root / name
        count = 0
        for directory, children, files in os.walk(source):
            children[:] = sorted(child for child in children if child != ".git" and not (Path(directory) / child).is_symlink())
            for filename in sorted(files):
                path = Path(directory) / filename
                if path.is_symlink() or not filename.upper().startswith(("LICENSE", "NOTICE", "COPYING", "COPYRIGHT", "AUTHORS")):
                    continue
                if not path.is_file() or path.stat().st_size > 1024 * 1024:
                    raise ScaffoldError("unexpected upstream license-file shape")
                destination = notices / "dependencies" / name / path.relative_to(source)
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(path, destination)
                count += 1
        if count == 0:
            raise ScaffoldError("upstream public notices are missing")
    compiler_packages = ("gcc-12-base", "libgcc-12-dev", "libstdc++-12-dev")
    compiler_versions = {}
    for name in compiler_packages:
        copyright_path = Path("/usr/share/doc") / name / "copyright"
        destination = notices / "dependencies/gcc-runtime" / f"{name}-copyright"
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(copyright_path, destination)
        compiler_versions[name] = subprocess.run(["dpkg-query", "-W", "-f=${Version}", name],
                                                check=True, text=True, capture_output=True, timeout=15).stdout
    evidence = output / "evidence"
    evidence.mkdir()
    shutil.copy2(test_report, evidence / "ctest-results.xml")
    manifest = {
        "schemaVersion": 1, "kind": PRODUCT_KIND, "sourceRevision": source_revision,
        "sourceDateEpoch": source_date_epoch, "architecture": "arm64", "os": "linux",
        "productTarget": "BHS_BUILD_KUKSA_RUNTIME=ON", "tests": tests, "binaries": binaries,
        "functionalProfile": functional_profile,
        "dependencies": dependencies, "compilerRuntimePackages": compiler_versions,
        "baseImage": "debian:bookworm-slim@sha256:6bd27d44e6c32a66bbd72d7cb2b76a8ae3497ec2e5274a81abd1b37f6013fa1f",
        "aptSnapshot": "20260901T000000Z",
        "liveQualified": False,
    }
    (output / "product-build.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    for path in output.rglob("*"):
        if path.is_file():
            os.utime(path, (source_date_epoch, source_date_epoch))
    return output


def build_scaffold(output: Path) -> Path:
    output = output.resolve()
    if output.exists():
        raise ScaffoldError(f"output already exists: {output}")

    rootfs = output / "service/arm64"
    executable_target = rootfs / "usr/bin/brake-health-service"
    config_target = rootfs / "etc/brake-health-service/compatibility.json"
    license_target = rootfs / "usr/share/licenses/brake-health-service"

    output.mkdir(parents=True)
    executable_target.parent.mkdir(parents=True)
    config_target.parent.mkdir(parents=True)
    license_target.mkdir(parents=True)

    shutil.copy2(ROOT / "packaging/aos/config.yaml", output / "config.yaml")
    shutil.copy2(ROOT / "src/usr/bin/brake-health-service", executable_target)
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
    parser.add_argument("--runtime-root", type=Path)
    parser.add_argument("--source-revision")
    parser.add_argument("--source-date-epoch", type=int)
    parser.add_argument("--functional-profile", choices=("v1", "v2", "v3"), default="v1")
    parser.add_argument("--dependency-source-root", type=Path)
    parser.add_argument("--test-report", type=Path)
    args = parser.parse_args()
    try:
        if args.runtime_root is not None:
            if not all((args.source_revision, args.source_date_epoch, args.dependency_source_root, args.test_report)):
                raise ScaffoldError("runtime export requires revision, timestamp, dependency source and CTest report")
            output = export_runtime(args.runtime_root, args.output, args.source_revision,
                                    args.source_date_epoch, args.dependency_source_root, args.test_report, args.functional_profile)
            print(f"Verified Linux ARM64 product exported: {output}")
            print("No signing, upload, credentials, runtime bindings or live qualification was performed.")
            return 0
        if any((args.source_revision, args.source_date_epoch, args.dependency_source_root, args.test_report)):
            raise ScaffoldError("product export arguments require --runtime-root")
        output = build_scaffold(args.output)
    except (OSError, ScaffoldError, json.JSONDecodeError, ET.ParseError, ValueError, subprocess.SubprocessError) as exc:
        print(f"Scaffold build failed: {exc}", file=sys.stderr)
        return 1
    print(f"Unsigned ARM64 Aos service scaffold created: {output}")
    print("No certificate, signature, cloud identity, or telemetry behavior was added.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
