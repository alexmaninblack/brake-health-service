# SPDX-FileCopyrightText: 2026 maninblack
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RUNTIME_ROOTS = (ROOT / "src", ROOT / "config", ROOT / "packaging")
FORBIDDEN_PATTERNS = {
    "CARLA import": re.compile(r"(?im)^\s*(?:from\s+carla\s+import|import\s+carla\b)"),
    "CARLA endpoint": re.compile(r"(?:127\.0\.0\.1|localhost):2000\b", re.IGNORECASE),
    "VISS endpoint": re.compile(r"(?:127\.0\.0\.1|localhost):3000\b", re.IGNORECASE),
    "platform provider source": re.compile(r"aos-vehicle-platform/(?:providers|authorization)/", re.IGNORECASE),
}


class RepositoryBoundaryTests(unittest.TestCase):
    def test_runtime_files_do_not_cross_platform_boundary(self) -> None:
        violations: list[str] = []
        for root in RUNTIME_ROOTS:
            for path in root.rglob("*"):
                if not path.is_file():
                    continue
                text = path.read_text(encoding="utf-8")
                for label, pattern in FORBIDDEN_PATTERNS.items():
                    if pattern.search(text):
                        violations.append(f"{path.relative_to(ROOT)}: {label}")
        self.assertEqual([], violations)

    def test_service_declares_only_read_only_kuksa_resource(self) -> None:
        config = (ROOT / "packaging/aos/config.yaml").read_text(encoding="utf-8")
        self.assertIn("architecture: arm64", config)
        self.assertIn("name: kuksa", config)
        self.assertNotRegex(config, r"mode: rw\b")
        self.assertNotRegex(config, r"mode: w\b")
        self.assertIn("dependencies: []", config)


if __name__ == "__main__":
    unittest.main()
