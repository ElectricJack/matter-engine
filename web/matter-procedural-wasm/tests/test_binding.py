"""Run the unchanged native binding ownership/error suite against this package build."""
from pathlib import Path
import subprocess


def test_existing_binding_contract():
    root = Path(__file__).resolve().parents[3]
    subprocess.run(['node', str(root / 'bindings/geometry-c/tests/wasm.mjs'),
                    str(root / 'web/matter-procedural-wasm/dist/matter_geometry_wasm.mjs')],
                   check=True, timeout=120)
