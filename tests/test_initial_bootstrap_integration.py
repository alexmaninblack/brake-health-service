# SPDX-FileCopyrightText: 2026 maninblack
# SPDX-License-Identifier: Apache-2.0
"""Linux container-only process gate; no IAM, Cloud, or live credentials."""
import json
import os
from pathlib import Path
import select
import signal
import subprocess
import tempfile
import time
import unittest

TEAM = "brake"
ENABLED = (os.environ.get("AOS_BOOTSTRAP_ISOLATED_TEST") == "1"
           and Path("/.dockerenv").exists() and os.geteuid() == 0)
BUILD = Path("/checks") / TEAM
SYNTHETIC_SECRET = "isolated-bootstrap-fixture-not-a-credential"


@unittest.skipUnless(ENABLED, "explicit isolated Linux container required")
class InitialBootstrapTests(unittest.TestCase):
    def setUp(self):
        release_dir = Path("/usr/share/aosedge")
        release_dir.mkdir(parents=True, exist_ok=True)
        (release_dir / "service-release.json").write_text(
            '{"schemaVersion":1,"serviceVersion":"21.0.0"}')
        token_root = Path("/run/aosedge/secrets/kuksa")
        token_root.mkdir(parents=True, exist_ok=True)
        token_root.chmod(0o1777)
        executable = Path("/usr/bin") / (TEAM + "-health-service")
        if not executable.exists():
            executable.symlink_to(BUILD / (TEAM + "-health-service"))
        self.tmp = tempfile.TemporaryDirectory(prefix="bootstrap-inputs-")
        self.root = Path(self.tmp.name)
        self.metadata = self.root / "metadata.json"
        self.ca = self.root / "trust.pem"
        self.process = None
        self.output = b""
        self.environment = dict(os.environ, AOS_ITEM_ID="fixture-service",
            AOS_SUBJECT_ID="fixture-subject", AOS_INSTANCE_INDEX="0",
            AOS_INSTANCE_ID="fixture-instance", AOS_SECRET=SYNTHETIC_SECRET)

    def tearDown(self):
        if self.process:
            if self.process.poll() is None:
                self.process.terminate()
            try:
                tail, _ = self.process.communicate(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                tail, _ = self.process.communicate(timeout=5)
                self.fail("bootstrap did not stop its child within its deadline")
            self.output += tail
            self.assertNotIn(SYNTHETIC_SECRET.encode(), self.output)
        self.tmp.cleanup()

    def publish_metadata(self, content=None):
        value = dict(schemaVersion=2, unitSystemUid="fixture-unit", unitRole="validation",
                     vdpContractVersion="1.0.1", vdpContractSha256="b" * 64)
        self.metadata.write_text(json.dumps(value) if content is None else content)

    def start(self):
        self.process = subprocess.Popen(
            [str(BUILD / (TEAM + "-health-bootstrap")), "--metadata-file",
             str(self.metadata), "--ca-file", str(self.ca)], env=self.environment,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    def await_text(self, text, seconds=8):
        wanted = text.encode()
        deadline = time.monotonic() + seconds
        while wanted not in self.output and time.monotonic() < deadline:
            available, _, _ = select.select([self.process.stdout], [], [], 0.1)
            if available:
                chunk = os.read(self.process.stdout.fileno(), 4096)
                self.output += chunk
                if not chunk:
                    break
        self.assertIn(wanted, self.output)
        return self.output

    def test_wait_and_automatic_child_start_after_both_inputs_arrive(self):
        self.start()
        self.await_text("INITIAL_PUBLIC_INPUTS_MISSING")
        self.assertIsNone(self.process.poll())
        self.publish_metadata()
        time.sleep(1.1)
        self.assertIsNone(self.process.poll())
        self.ca.write_text("nonempty-public-trust-fixture-no-live-cert")
        self.await_text('"currentState":"AVAILABLE"')
        self.await_text("SERVICE_STARTED")
        self.assertIsNone(self.process.poll())
        # No KAC exists: the child remains fail-closed, with no issued token.
        self.assertIn(b"KUKSA_AUTH_UNAVAILABLE", self.await_text("KUKSA_AUTH_UNAVAILABLE"))
        for path in Path("/run/aosedge/secrets/kuksa").glob("session-*/token.jwt"):
            self.fail("fixture unexpectedly issued a token")
        self.assertEqual(self.output.count(b"INITIAL_PUBLIC_INPUTS_MISSING"), 1)

    def test_missing_native_identity_is_not_waiting(self):
        self.environment.pop("AOS_INSTANCE_ID")
        self.start()
        self.assertEqual(self.process.wait(timeout=5), 2)

    def test_invalid_metadata_is_not_waiting(self):
        self.publish_metadata("{}")
        self.start()
        self.assertEqual(self.process.wait(timeout=5), 2)

    def test_empty_trust_is_not_waiting(self):
        self.publish_metadata()
        self.ca.write_text("")
        self.start()
        self.assertEqual(self.process.wait(timeout=5), 2)

    def test_waiting_process_stops_cleanly(self):
        self.start()
        self.await_text("INITIAL_PUBLIC_INPUTS_MISSING")
        self.process.send_signal(signal.SIGTERM)
        self.assertEqual(self.process.wait(timeout=5), 0)


if __name__ == "__main__":
    unittest.main()
