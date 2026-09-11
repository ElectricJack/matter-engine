"""Reject incomplete or changed supplemental connector proof, without an editor."""

import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    "castle_batch", ROOT / "tools/castle_site_walkthrough_batch.py"
)
batch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(batch)
driver = batch.load_driver()


class ConnectorProofTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.path = Path(self.temporary.name) / "override.json"
        self.sequence = [[0, 0, 0], [1, 0, 0], [1, 0, 3], [2, 0, 3]]
        self.manifest = {
            "connectors": [{"id": "bend", "routeWaypoints": self.sequence}]
        }
        self.portals = [
            {"id": "connector:bend:mouth:" + side, "connector_id": "bend"}
            for side in ("a", "b")
        ]
        self.normalized = {"portals": self.portals}
        self.path.write_text(
            json.dumps({"connector_id": "bend", "source_manifest_sha256": "a" * 64})
        )
        self.plan = {
            "manifest_sha256": "a" * 64,
            "portals": self.portals,
            "waypoints": self.sequence + self.sequence[-2::-1],
            "route_override_sha256": batch.sha(self.path),
        }

    def verify(self, plan):
        batch.verify_connector_detour(
            driver, self.manifest, self.normalized, "bend", plan, self.path
        )

    def test_complete_round_trip_and_both_mouths(self):
        self.verify(self.plan)

    def test_one_mouth_cannot_satisfy_connector_proof(self):
        plan = copy.deepcopy(self.plan)
        plan["portals"].pop()
        with self.assertRaisesRegex(RuntimeError, "both selected finite mouth"):
            self.verify(plan)

    def test_return_cannot_shortcut_the_authored_bend(self):
        plan = copy.deepcopy(self.plan)
        plan["waypoints"].pop(4)
        with self.assertRaisesRegex(RuntimeError, "shortened"):
            self.verify(plan)

    def test_wrong_manifest_or_changed_override_fails(self):
        plan = copy.deepcopy(self.plan)
        plan["manifest_sha256"] = "b" * 64
        with self.assertRaisesRegex(RuntimeError, "provenance"):
            self.verify(plan)
        route = {
            "route_override": str(self.path),
            "route_override_sha256": batch.sha(self.path),
        }
        batch.verify_override_input(route)
        self.path.write_text(self.path.read_text() + "\n")
        with self.assertRaisesRegex(RuntimeError, "changed"):
            batch.verify_override_input(route)


if __name__ == "__main__":
    unittest.main()
