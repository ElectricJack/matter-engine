"""Run the focused real browser gate through AQ's test semaphore."""
from pathlib import Path
import subprocess


def test_procedural_demo_browser():
    app = Path(__file__).resolve().parents[1]
    subprocess.run(["node", "frontend-tests/browser.mjs"], cwd=app, check=True, timeout=180)


def test_gallery_smoke():
    app = Path(__file__).resolve().parents[1]
    subprocess.run(["node", "frontend-tests/smoke.mjs"], cwd=app, check=True, timeout=25)


def test_frontend_units():
    app = Path(__file__).resolve().parents[1]
    subprocess.run(["npm", "test"], cwd=app, check=True, timeout=30)
