"""CPU regression for the actual counter reporter and submission commit function.

Only buffer I/O, logging, and unrelated renderer state are stubbed. This does
not exercise a GPU writer, frame fences, allocation, reset, or RT availability;
the native rt-counters smoke mode remains responsible for those paths.
Run through resource admission: aq test -n 0 MatterEngine3/tests/test_rt_counter_cpu.py
"""
from pathlib import Path
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / "libs/MatterGraphics/src/native"


@pytest.fixture(scope="module")
def counter_probe(tmp_path_factory):
    source = (NATIVE / "rt_dispatch.inl").read_text()
    start = source.index("void VkSceneRenderer::report_rt_trace_counters(")
    end = source.index("\n// Turns the selected geometry", start)
    reporter = source[start:end]
    contract = (NATIVE / "rt_contract.inl").read_text()
    counters = contract[:contract.index("\nstruct alignas")]
    # Compile the whole production commit implementation, not a rewritten
    # counter state machine. Unrelated histories and BLAS state stay empty.
    commit = (NATIVE / "rt_frame_commit.inl").read_text()
    fixture = Path(__file__).with_name("rt_counter_cpu_fixture.cpp").read_text()
    assert fixture.count("// PRODUCTION_FUNCTIONS") == 1
    directory = tmp_path_factory.mktemp("rt-counter-cpu")
    cpp = directory / "probe.cpp"
    cpp.write_text(fixture.replace("// PRODUCTION_FUNCTIONS", counters + reporter + commit))
    exe = directory / "probe"
    build = subprocess.run(
        ["g++", "-std=c++17", "-O0", "-Wall", "-Wextra", str(cpp), "-o", str(exe)],
        capture_output=True, text=True, timeout=60,
    )
    assert build.returncode == 0, build.stdout + build.stderr
    return exe


@pytest.mark.parametrize("case", [
    "poison", "equal", "stale", "submission", "duplicate",
    "zero", "map-failure", "invalidate-failure", "missing-buffer",
])
def test_counter_accounting(counter_probe, case):
    result = subprocess.run(
        [str(counter_probe), case], capture_output=True, text=True, timeout=10,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.strip() == f"PASS {case}"
