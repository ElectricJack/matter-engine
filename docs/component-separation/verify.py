#!/usr/bin/env python3
"""Check the pinned extraction record without building or running native code."""

import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
DOCS = Path(__file__).resolve().parent


def git(*args):
    return subprocess.check_output(["git", *args], cwd=ROOT)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def verify():
    baseline = json.loads((DOCS / "baseline.json").read_text())
    fixtures = json.loads((DOCS / "fixtures.json").read_text())
    commit = baseline["live_commit"]
    assert fixtures["baseline_commit"] == commit, "fixture baseline mismatch"
    git("cat-file", "-e", commit + "^{commit}")
    blobs = {}

    def source(path):
        assert not Path(path).is_absolute() and ".." not in Path(path).parts, path
        if path not in blobs:
            blobs[path] = git("show", f"{commit}:{path}")
        return blobs[path]

    for spec in baseline["spec_sources"]:
        data = (ROOT / spec["repository_path"]).read_bytes()
        assert len(data) == spec["bytes"], spec["repository_path"]
        assert digest(data) == spec["sha256"], spec["repository_path"]
        assert f'status: {spec["status_preserved"]}\n'.encode() in data

    pinned = {item["path"]: item["sha256"] for item in baseline["pinned_sources"]}
    assert len(pinned) == len(baseline["pinned_sources"]), "duplicate pinned path"
    for path, expected in pinned.items():
        assert digest(source(path)) == expected, f"pinned source hash: {path}"

    engine = source("cmake/MatterEngine.cmake").decode()
    viewer = source("cmake/MatterViewer.cmake").decode()
    native_targets = set(re.findall(r"matter_add_engine_cpu_test\(\s*(\w+)", engine))
    native_targets.add("vulkan_smoke_tests")
    explicit_ctests = set(re.findall(r"add_test\(NAME\s+(\w+)", engine + viewer))
    ctests = native_targets | explicit_ctests
    mode_list = re.search(r"foreach\(mode\s+([^)]*)\)", viewer).group(1).split()
    ctests.update("smoke_" + mode.replace("-", "_") for mode in mode_list)
    ids = []
    for fixture in fixtures["fixtures"]:
        ids.append(fixture["id"])
        assert fixture["sources"] and fixture["expected"], fixture["id"]
        assert fixture["new_contract_gate_gap"], fixture["id"]
        for path in fixture["sources"]:
            assert path in pinned, f'{fixture["id"]}: unpinned {path}'
        for path, anchors in fixture.get("source_anchors", {}).items():
            assert path in fixture["sources"], f"anchor source absent: {path}"
            for anchor in anchors:
                assert anchor in source(path).decode(), f"anchor missing: {path}: {anchor}"
        for target in fixture["native_targets"]:
            assert target in native_targets, f"unknown native target: {target}"
        for test in fixture["ctest_names"]:
            assert test in ctests, f"unknown CTest: {test}"
        for command in fixture["node_commands"]:
            assert command.startswith("node "), command
            assert command[5:] in fixture["sources"], command
    assert len(ids) == len(set(ids)), "duplicate fixture ID"

    changed = git("diff", "--name-only", baseline["design_inspected_commit"], commit)
    assert changed.decode().splitlines() == baseline["reconciliation"]["changed_paths"]
    assert len(changed.decode().splitlines()) == baseline["reconciliation"]["changed_path_count"]
    for label, name, prefix in (
            ("core", "engine-core", "MatterEngine3/src/"),
            ("surface", "matter-surface", "libs/MatterSurfaceLib/src/"),
            ("viewer", "engine-viewer", "MatterEngine3/src/")):
        entries = source(f"cmake/manifests/{name}.sources").decode().splitlines()
        count = sum(1 for line in entries if line and not line.startswith("#")
                    and line.split("|")[1] in ("all", "windows")
                    and line.split("|")[0].startswith(prefix)
                    and line.split("|")[0].endswith((".c", ".cpp"))
                    and line.split("|")[0] != "MatterEngine3/src/inotify_watcher.cpp")
        assert count == baseline["reconciliation"]["current_source_counts"][label]
    presets = json.loads(source("CMakePresets.json"))
    preset = next(p for p in presets["configurePresets"]
                  if p["name"] == "windows-msvc-relwithdebinfo")
    assert preset["binaryDir"].replace("${sourceDir}/", "") in (DOCS / "validation.md").read_text()
    print(f"PASS: 2 exact spec copies, {len(pinned)} pinned sources, "
          f"{len(ids)} fixtures and native target/CTest registrations at {commit}")
    print("Native builds, GPU execution, new portable contracts and WASM parity are not checked.")


if __name__ == "__main__":
    try:
        verify()
    except (AssertionError, OSError, ValueError, KeyError, StopIteration,
            subprocess.CalledProcessError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(1)
