# SPDX-FileCopyrightText: 2026 maninblack
# SPDX-License-Identifier: Apache-2.0

"""Isolated exporter/recipe tests; no Docker or product runtime execution."""

from __future__ import annotations

import importlib.util
import struct
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("product_export", ROOT / "tools/build_scaffold.py")
assert SPEC is not None and SPEC.loader is not None
EXPORT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(EXPORT)


class ProductExportTests(unittest.TestCase):
    def test_rejects_diagnostic_stub_before_readelf(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "brake-health-service"
            path.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            path.chmod(0o755)
            with mock.patch.object(EXPORT, "_readelf") as readelf:
                with self.assertRaises(EXPORT.ScaffoldError):
                    EXPORT.inspect_product_elf(path)
                readelf.assert_not_called()

    def test_elf_shape_and_dynamic_closure_fail_closed(self) -> None:
        # Deliberately synthetic parser fixture: never exported or executed.
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "brake-health-service"
            header = bytearray(64)
            header[:7] = b"\x7fELF\x02\x01\x01"
            struct.pack_into("<HHI", header, 16, 3, 183, 1)
            path.write_bytes(header)
            path.chmod(0o755)
            results = {
                "--program-headers": "[Requesting program interpreter: /lib/ld-linux-aarch64.so.1]",
                "--dynamic": "(NEEDED) Shared library: [libc.so.6]\n(NEEDED) Shared library: [libm.so.6]",
                "--version-info": "Name: GLIBC_2.17\nName: GLIBC_2.34",
            }
            with mock.patch.object(EXPORT, "_readelf", side_effect=lambda _path, option: results[option]):
                observed = EXPORT.inspect_product_elf(path)
                self.assertEqual("rootfs/usr/bin/brake-health-service", observed["path"])
                self.assertEqual(["libc.so.6", "libm.so.6"], observed["needed"])
                self.assertEqual(64, observed["size"])
                results["--dynamic"] += "\n(NEEDED) Shared library: [libgrpc++.so.1]"
                with self.assertRaises(EXPORT.ScaffoldError):
                    EXPORT.inspect_product_elf(path)
                results["--dynamic"] = "(NEEDED) Shared library: [libc.so.6]\n(RUNPATH) Library runpath: [/opt/bhs]"
                with self.assertRaises(EXPORT.ScaffoldError):
                    EXPORT.inspect_product_elf(path)
                results["--dynamic"] = "(NEEDED) Shared library: [libc.so.6]"
                results["--version-info"] = "Name: GLIBC_2.38"
                with self.assertRaises(EXPORT.ScaffoldError):
                    EXPORT.inspect_product_elf(path)
                struct.pack_into("<H", header, 18, 62)
                path.write_bytes(header)
                with self.assertRaises(EXPORT.ScaffoldError):
                    EXPORT.inspect_product_elf(path)

    def test_requires_actual_complete_successful_ctest_report(self) -> None:
        names = ("native_service_inputs", "brake_private_token_session",
                 "brake_health_v1_contract", "brake_health_v2_contract",
                 "brake_health_runtime_contract", "brake_health_application_contract",
                 "brake_demo_mock_isolation", "function_observation_delivery")
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "report.xml"
            body = '<testsuite tests="8" failures="0">' + "".join(
                f'<testcase name="{name}" />' for name in names) + "</testsuite>"
            path.write_text(body, encoding="utf-8")
            self.assertEqual("passed", EXPORT.inspect_test_report(path)["ctest"])
            self.assertEqual(8, EXPORT.inspect_test_report(path)["count"])
            path.write_text(body.replace('<testcase name="function_observation_delivery" />', ''), encoding="utf-8")
            with self.assertRaises(EXPORT.ScaffoldError):
                EXPORT.inspect_test_report(path)
            path.write_text(body.replace('failures="0"', 'failures="1"'), encoding="utf-8")
            with self.assertRaises(EXPORT.ScaffoldError):
                EXPORT.inspect_test_report(path)
            path.write_text(body.replace('name="brake_health_v1_contract" />',
                                         'name="brake_health_v1_contract"><skipped /></testcase>'), encoding="utf-8")
            with self.assertRaises(EXPORT.ScaffoldError):
                EXPORT.inspect_test_report(path)
            path.write_text('<testsuite tests="0" failures="0" />', encoding="utf-8")
            with self.assertRaises(EXPORT.ScaffoldError):
                EXPORT.inspect_test_report(path)

    def test_product_recipe_pins_real_target_and_export(self) -> None:
        dockerfile = (ROOT / "Dockerfile").read_text(encoding="utf-8")
        self.assertIn("debian:bookworm-slim@sha256:6bd27d44e6c32a66bbd72d7cb2b76a8ae3497ec2e5274a81abd1b37f6013fa1f", dockerfile)
        self.assertIn("20260901T000000Z", dockerfile)
        for name in ("grpc", "protobuf", "abseil", "kuksa"):
            self.assertIn(EXPORT.DEPENDENCY_REVISIONS[name], dockerfile)
        self.assertIn(EXPORT.OPENSSL_ARCHIVE_SHA256, dockerfile)
        self.assertIn("-DBHS_BUILD_KUKSA_RUNTIME=ON", dockerfile)
        self.assertIn("--output-junit /build/service/ctest-results.xml", dockerfile)
        self.assertIn("--runtime-root /build/rootfs", dockerfile)
        self.assertIn("FROM scratch AS export", dockerfile)
        self.assertNotIn("src/usr/bin/brake-health-service", dockerfile)
        self.assertNotIn("chmod 777", dockerfile)
        self.assertNotIn("--insecure", dockerfile)
        self.assertNotIn("ENTRYPOINT", dockerfile)


if __name__ == "__main__":
    unittest.main()
