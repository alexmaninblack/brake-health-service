# SPDX-FileCopyrightText: 2026 maninblack
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class BrakeHealthV2CppTests(unittest.TestCase):
    def test_configure_build_and_v2_ctest_without_downloads(self) -> None:
        cmake = shutil.which("cmake")
        self.assertIsNotNone(cmake, "CMake is required by WP-P1-BHS-CORE-002")
        assert cmake is not None

        with tempfile.TemporaryDirectory(prefix="brake-health-v2-build-") as temp_dir:
            build = Path(temp_dir) / "build"
            subprocess.run(
                [
                    cmake,
                    "-S",
                    str(ROOT),
                    "-B",
                    str(build),
                    "-DCMAKE_BUILD_TYPE=Debug",
                    "-DBUILD_TESTING=ON",
                ],
                check=True,
            )
            subprocess.run(
                [cmake, "--build", str(build), "--parallel", "2"],
                check=True,
            )
            subprocess.run(
                [
                    "ctest",
                    "--test-dir",
                    str(build),
                    "--output-on-failure",
                    "-R",
                    "brake_health_v2_contract",
                ],
                check=True,
            )


if __name__ == "__main__":
    unittest.main()
