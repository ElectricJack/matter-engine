"""AQ/pytest entry point for the built bundle's fast Chromium smoke.

Set MATTER_WEB_BUNDLE_DIR and, if needed, MATTER_WEB_TEST_PYTHON/BROWSER.
The subprocess uses the Python environment with Playwright installed.
"""
import json
import os
from pathlib import Path
import subprocess
import sys


def test_built_demo_frame(tmp_path):
    bundle = os.environ.get("MATTER_WEB_BUNDLE_DIR")
    assert bundle, "build matter_web_demo and set MATTER_WEB_BUNDLE_DIR"
    output = tmp_path / "smoke.json"
    command = [os.environ.get("MATTER_WEB_TEST_PYTHON", sys.executable),
               str(Path(__file__).with_name("demo_bundle.py")), "--bundle-dir", bundle,
               "--browser", os.environ.get("MATTER_WEB_TEST_BROWSER", "/opt/google/chrome/chrome"),
               "--output", str(output)]
    completed = subprocess.run(command, capture_output=True, text=True, timeout=10)
    assert completed.returncode == 0, completed.stdout + completed.stderr
    evidence = json.loads(output.read_text())
    assert evidence["result"] == "pass", evidence
