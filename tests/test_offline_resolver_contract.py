# SPDX-FileCopyrightText: 2026 maninblack
# SPDX-License-Identifier: Apache-2.0
"""Source contract only; offline reconnection requires a live container proof."""
import unittest
from pathlib import Path


class OfflineResolverTests(unittest.TestCase):
    def test_native_hosts_resolution_is_selected_before_grpc_process_starts(self):
        source = (Path(__file__).resolve().parents[1] / "src/runtime/bootstrap_main.cpp").read_text()
        selection = '::setenv("GRPC_DNS_RESOLVER", "native", 1)'
        self.assertIn(selection, source)
        self.assertLess(source.index(selection), source.index("::fork()"))
        self.assertIn("LOCAL_RESOLVER_CONFIGURATION_INVALID", source)
        runtime = (Path(__file__).resolve().parents[1] / "src/runtime/grpc_main.cpp").read_text()
        self.assertIn("Server:55555", runtime)
        self.assertNotIn("InsecureChannelCredentials", runtime)


if __name__ == "__main__":
    unittest.main()
