# SPDX-FileCopyrightText: 2026 maninblack
# SPDX-License-Identifier: Apache-2.0
"""Source guard for the pinned VAL Subscribe request, not a live RPC proof."""
import unittest
from pathlib import Path


class SubscribeContractTests(unittest.TestCase):
    def test_timing_diagnostic_does_not_log_every_jitter_transition(self):
        source = (Path(__file__).resolve().parents[1] / "src/runtime/grpc_main.cpp").read_text()
        diagnostic = source.split("void input_timing(", 1)[1].split("void input_rejected(", 1)[0]
        self.assertIn("if(bucket<=largest_timing_bucket_)return;", diagnostic)
        self.assertLess(diagnostic.index("largest_timing_bucket_=bucket;"), diagnostic.index('state("KUKSA_INPUT_TIMING"'))

    def test_gateway_status_subscription_explicitly_requests_read_only_value(self):
        source = (Path(__file__).resolve().parents[1] / "src/runtime/grpc_main.cpp").read_text()
        request = source.split("val::SubscribeRequest subscribe_request;", 1)[1].split("auto reader_context", 1)[0]
        self.assertIn("wanted->set_path(brake_health::v3::kStatusPath)", request)
        self.assertIn("wanted->add_fields(val::FIELD_VALUE)", request)
        self.assertNotIn("FIELD_ACTUATOR_TARGET", request)

    def test_telemetry_subscription_explicitly_requests_value_fields(self):
        source = (Path(__file__).resolve().parents[1] / "src/runtime/grpc_main.cpp").read_text()
        request = source.split("val::SubscribeRequest subscription;", 1)[1].split("auto stream_context", 1)[0]
        self.assertIn("for (const auto& path : telemetry_paths())", request)
        self.assertIn("entry->set_path(path)", request)
        self.assertIn("entry->add_fields(val::FIELD_VALUE)", request)
        self.assertNotIn("FIELD_ACTUATOR_TARGET", request)


if __name__ == "__main__":
    unittest.main()
