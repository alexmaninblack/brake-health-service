# SPDX-FileCopyrightText: 2026 maninblack
# SPDX-License-Identifier: Apache-2.0
"""Source guard for the pinned VAL Subscribe request, not a live RPC proof."""
import unittest
from pathlib import Path


class SubscribeContractTests(unittest.TestCase):
    def test_subscription_failures_use_the_tested_observation_mapping(self):
        source = (Path(__file__).resolve().parents[1] / "src/runtime/grpc_main.cpp").read_text()
        self.assertIn("const auto observation = subscription_failure_observation(code);", source)
        self.assertIn("runtime.input_observation(observation.connection, observation.input, observation.reason);", source)
        self.assertIn("grpc::StatusCode::UNAUTHENTICATED", source)
        self.assertIn("grpc::StatusCode::PERMISSION_DENIED", source)

    def test_renewal_recreates_stream_without_skipping_authentication(self):
        source = (Path(__file__).resolve().parents[1] / "src/runtime/grpc_main.cpp").read_text()
        self.assertIn("inspect_session_inputs(inputs, token_file, token, metadata_bytes, ca)", source)
        self.assertIn("catch (const ReauthenticationRequired&)", source)
        self.assertIn("runtime.disconnect();", source)
        self.assertIn("grpc::StatusCode::CANCELLED", source)
        self.assertIn("grpc::StatusCode::UNAUTHENTICATED", source)
        self.assertIn("pause(stop, 1000);", source)  # Real failures retain backoff.
        renewal = source.rsplit("catch (const ReauthenticationRequired&)", 1)[1].split("catch (const std::exception&", 1)[0]
        self.assertIn("continue;", renewal)
        self.assertNotIn("pause(", renewal)

    def test_episode_outcomes_have_a_separate_repeatable_diagnostic_budget(self):
        source = (Path(__file__).resolve().parents[1] / "src/runtime/grpc_main.cpp").read_text()
        logger = source.split("void state(", 1)[1].split("void pause(", 1)[0]
        for event in ("ASSESSMENT_CREATED", "ASSESSMENT_SKIPPED_INPUT_QUALITY", "WINDOW_COMPLETED"):
            self.assertIn('event == "' + event + '"', logger)
        self.assertIn("if (!occurrence && previous_[event] == key) return;", logger)
        self.assertIn("occurrence ? occurrence_emitted_ : emitted_", logger)

    def test_repeated_input_failures_have_bounded_non_payload_diagnostics(self):
        source = (Path(__file__).resolve().parents[1] / "src/runtime/grpc_main.cpp").read_text()
        diagnostic = source.split("void input_rejected(", 1)[1].split("void analytics(", 1)[0]
        self.assertIn("next_input_report_ = now + 10000", diagnostic)
        self.assertIn("missing_mask", diagnostic)
        self.assertNotIn(".value", diagnostic)
        self.assertIn("if (!freshness_expired) log.state", source)

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
