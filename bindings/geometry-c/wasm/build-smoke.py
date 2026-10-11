#!/usr/bin/env python3
"""Reproduce the pinned contract smoke in a new build directory, without installs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys


def main():
    here = Path(__file__).resolve().parent
    lock = json.loads((here / "toolchain.json").read_text())
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emsdk", type=Path, default=os.environ.get("EMSDK"))
    parser.add_argument("--build-dir", required=True, type=Path, help="must be absent or empty")
    args = parser.parse_args()
    if args.emsdk is None:
        parser.error("pass --emsdk or activate EMSDK first")
    sdk = args.emsdk.resolve()
    build = args.build_dir.resolve()
    if build.exists() and (not build.is_dir() or any(build.iterdir())):
        parser.error("clean build requires an absent or empty --build-dir; no files are deleted")
    node_dir = sdk / "node" / f"{lock['node']}_64bit" / "bin"
    if os.name == "nt":
        node_dir = node_dir.parent
    env = os.environ.copy()
    env["PATH"] = os.pathsep.join([str(node_dir), str(sdk / "upstream" / "emscripten"), env.get("PATH", "")])
    env["EM_CONFIG"] = str(sdk / ".emscripten")
    # Private cache makes this a clean SDK/sysroot build as well as a clean project build.
    env["EM_CACHE"] = str(build / "emscripten-cache")
    observed = {}
    for tool in ("emcc", "node", "cmake", "ninja"):
        executable = shutil.which(tool, path=env["PATH"])
        if executable is None:
            parser.error(f"required tool unavailable: {tool}")
        output = subprocess.check_output([executable, "--version"], env=env, text=True, stderr=subprocess.STDOUT)
        key = "emscripten" if tool == "emcc" else tool
        match = re.search(r"\b\d+\.\d+\.\d+\b", output)
        # Node prefixes its version with 'v'.
        version = output.strip().lstrip("v") if tool == "node" else (match.group(0) if match else "")
        if version != lock[key] or (tool == "emcc" and lock["emscripten_revision"] not in output):
            parser.error(f"{tool} requires {lock[key]}, observed {output.strip()}")
        observed[key] = {"version": version, "path": executable}
    if platform.python_version() != lock["python"]:
        parser.error(f"Python requires {lock['python']}, observed {platform.python_version()}")
    build.mkdir(parents=True, exist_ok=True)
    commands = [
        ["cmake", "-S", str(here.parent), "-B", str(build), "-G", "Ninja",
         f"-DCMAKE_TOOLCHAIN_FILE={sdk / 'upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake'}",
         "-DCMAKE_BUILD_TYPE=Release", "-DMATTER_GEOMETRY_C_WASM_SMOKE=ON"],
        ["cmake", "--build", str(build), "--parallel", "1"],
        ["ctest", "--test-dir", str(build), "--output-on-failure", "--parallel", "1"],
    ]
    evidence = {"profile": lock["profile"], "emscripten_revision": lock["emscripten_revision"],
                "toolchain_lock_sha256": hashlib.sha256((here / "toolchain.json").read_bytes()).hexdigest(),
                "tools": observed,
                "python": platform.python_version(), "platform": platform.platform(),
                "commands": commands, "checks": [], "artifacts": {}}
    evidence_path = build / "smoke-evidence.json"
    try:
        for command in commands:
            print("RUN:", json.dumps(command), flush=True)
            result = subprocess.run(command, env=env, check=False)
            evidence["checks"].append({"command": command, "exit_code": result.returncode})
            if result.returncode:
                return result.returncode
        for artifact in sorted(build.glob("wasm_contract_*.wasm")):
            data = artifact.read_bytes()
            evidence["artifacts"][artifact.name] = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
        print(f"PASS: evidence {evidence_path}")
        return 0
    finally:
        evidence_path.write_text(json.dumps(evidence, indent=2) + "\n")


if __name__ == "__main__":
    sys.exit(main())
