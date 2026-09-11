"""CPU-only coverage of explicit reserves crossing the sanitized launch boundary."""

import importlib.util
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / "tools/castle_walkthrough_acceptance.py"
spec = importlib.util.spec_from_file_location("castle_walk_reserves", DRIVER)
walk = importlib.util.module_from_spec(spec)
spec.loader.exec_module(walk)


class ReserveTests(unittest.TestCase):
    def test_actual_launch_environment(self):
        with tempfile.TemporaryDirectory() as directory:
            args = SimpleNamespace(
                output=directory,
                timeout=10,
                editor="unused-editor.exe",
                world="CastleClusteredCourt",
                static_vertex_reserve_mb=4096,
                static_index_reserve_mb=512,
            )
            session = walk.NativeSession(args)
            fake = SimpleNamespace(stdout=io.StringIO(""))
            with (
                patch.dict(
                    os.environ,
                    {
                        "MATTER_SCREENSHOT": "stale.png",
                        "MATTER_VK_STATIC_RESERVE_VERTEX_MB": "1",
                    },
                ),
                patch.object(walk.subprocess, "Popen", return_value=fake) as spawn,
            ):
                session.launch()
                session.reader.join(timeout=2)
            env = spawn.call_args.kwargs["env"]
            self.assertEqual(env["MATTER_VK_STATIC_RESERVE_VERTEX_MB"], "4096")
            self.assertEqual(env["MATTER_VK_STATIC_RESERVE_INDEX_MB"], "512")
            self.assertEqual(env["MATTER_VK_VALIDATION"], "1")
            self.assertEqual(env["MATTER_HIDE_WINDOW"], "1")
            self.assertNotIn("MATTER_SCREENSHOT", env)
            self.assertEqual(walk.static_buffer_reserve_env(SimpleNamespace()), {})

    def test_cli_rejects_nonpositive_reserves_before_launch(self):
        for flag in ("--static-vertex-reserve-mb", "--static-index-reserve-mb"):
            result = subprocess.run(
                [sys.executable, str(DRIVER), flag, "0", "--prepare-only"],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 2)
            self.assertIn("positive integer", result.stderr)


if __name__ == "__main__":
    unittest.main()
