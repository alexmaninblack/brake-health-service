# SPDX-FileCopyrightText: 2026 maninblack
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import importlib.util
import os
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools/build_scaffold.py"
SPEC = importlib.util.spec_from_file_location("build_scaffold", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
BUILDER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BUILDER)


class ScaffoldTests(unittest.TestCase):
    def test_builds_unsigned_arm64_staging_tree(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            output = Path(temp_dir) / "aos-service"
            BUILDER.build_scaffold(output)

            executable = output / "service/arm64/usr/bin/brake-health-service"
            self.assertTrue(executable.is_file())
            self.assertTrue(os.access(executable, os.X_OK))
            self.assertTrue((output / "config.yaml").is_file())
            self.assertTrue(
                (output / "service/arm64/etc/brake-health-service/compatibility.json").is_file()
            )
            self.assertTrue(
                (output / "service/arm64/usr/share/licenses/brake-health-service/LICENSE").is_file()
            )
            self.assertEqual([], list(output.rglob("*.p12")))
            self.assertEqual([], list(output.rglob("*.pem")))

            result = subprocess.run(
                [str(executable)], check=True, text=True, capture_output=True
            )
            self.assertEqual(
                "Brake Health Service scaffold: analysis behavior is not implemented.\n",
                result.stdout,
            )

            config = (output / "config.yaml").read_text(encoding="utf-8")
            self.assertIn("codename: brake-health-service", config)
            self.assertIn("title: Brake Health Service", config)

    def test_refuses_to_replace_existing_output(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            output = Path(temp_dir) / "existing"
            output.mkdir()
            with self.assertRaises(BUILDER.ScaffoldError):
                BUILDER.build_scaffold(output)


if __name__ == "__main__":
    unittest.main()
