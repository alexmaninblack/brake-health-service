# SPDX-FileCopyrightText: 2026 maninblack
# SPDX-License-Identifier: Apache-2.0
"""Source guard for the pinned VAL Subscribe request, not a live RPC proof."""
import unittest
from pathlib import Path


class SubscribeContractTests(unittest.TestCase):
    def test_telemetry_subscription_explicitly_requests_value_fields(self):
        source = (Path(__file__).resolve().parents[1] / "src/runtime/grpc_main.cpp").read_text()
        request = source.split("val::SubscribeRequest subscription;", 1)[1].split("auto stream_context", 1)[0]
        self.assertIn("for (const auto& path : telemetry_paths())", request)
        self.assertIn("entry->set_path(path)", request)
        self.assertIn("entry->add_fields(val::FIELD_VALUE)", request)
        self.assertNotIn("FIELD_ACTUATOR_TARGET", request)


if __name__ == "__main__":
    unittest.main()
