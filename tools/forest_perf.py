#!/usr/bin/env python3
"""Measure the authored ConiferForest with native Windows Python.

Runs must be sequential on the same GPU. The editor waits for static uploads
to settle before warmup, then samples and exits. Raw timings, a profile trace,
a screenshot and the exact source/settings are retained in a fresh directory.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess


CAMERAS = {
    "overview": "58,36,78,0,20,0",
    "ground": "2,1.8,18,0,7,0",
    "distant": "100,65,145,0,20,0",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out-dir", required=True, type=Path)
    parser.add_argument("--camera", choices=CAMERAS, default="overview")
    parser.add_argument("--cache", type=Path, default=Path("C:/tmp/conifer-cache"))
    parser.add_argument("--width", type=int, default=1600)
    parser.add_argument("--height", type=int, default=1000)
    parser.add_argument("--warmup", type=float, default=8)
    parser.add_argument("--sample", type=float, default=12)
    parser.add_argument("--timeout", type=float, default=1800)
    parser.add_argument("--validation", action="store_true")
    parser.add_argument("--ray-tracing", choices=("on", "off"), default="on")
    parser.add_argument("--pixel-budget", type=float, default=1.0)
    parser.add_argument("--hidden", action="store_true")
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("Run with native Windows Python: py -3 tools/forest_perf.py ...")
    if not (args.width > 0 and args.height > 0 and args.warmup >= 5
            and args.sample > 0 and args.timeout > 0):
        parser.error("Positive size/sample/timeout and at least 5 seconds warmup are required")
    if not math.isfinite(args.pixel_budget) or not 0.05 <= args.pixel_budget <= 4.0:
        parser.error("Pixel budget must be finite and between 0.05 and 4.0")
    repo = Path(__file__).resolve().parent.parent
    editor = repo / "MatterEditor/build/windows-msvc/editor.exe"
    out = args.out_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    if any(out.iterdir()):
        parser.error("Use an empty output directory to preserve earlier measurements")
    commands = out / "commands.txt"
    commands.write_text(
        f"set viewer.budget.pixel_budget {args.pixel_budget}\nget viewer.budget.pixel_budget\n"
        f"wait_idle 2 1800\nwait_frames 8\nshot {(out / 'view.png').as_posix()}\n"
        "stats forest-settled\n", encoding="utf-8")
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith("MATTER_")}
    controls = {
        "MATTER_WORLD": "ConiferForest", "MATTER_CAM": CAMERAS[args.camera],
        "MATTER_CACHE_ROOT": args.cache.resolve().as_posix(),
        "MATTER_CMD_FIFO": commands.as_posix(),
        "MATTER_WINDOW_WIDTH": str(args.width), "MATTER_WINDOW_HEIGHT": str(args.height),
        "MATTER_FLATTEN_LADDER": "1", "MATTER_HIDE_UI": "1",
        # Global editor preferences survive launches. Pin this explicitly so
        # an interactive toggle cannot look like a renderer speedup.
        "MATTER_DISABLE_VK_RT": "0" if args.ray_tracing == "on" else "1",
        "MATTER_HIDE_WINDOW": str(int(args.hidden)),
        "MATTER_FRAME_LIMIT": "0", "MATTER_VSYNC": "0",
        "MATTER_PERF_OUTPUT": (out / "perf.json").as_posix(),
        "MATTER_PERF_WARMUP_SECONDS": str(args.warmup),
        "MATTER_PERF_SAMPLE_SECONDS": str(args.sample),
        "MATTER_PROFILE_TRACE": (out / "profile.json").as_posix(),
        "MATTER_FRAME_TIMINGS": "1",
    }
    if args.validation:
        controls["MATTER_VK_VALIDATION"] = "1"
    env.update(controls)
    sources = [repo / "projects/world_demo/shared-lib/conifer_forest.js",
               repo / "projects/world_demo/scenes/vegetation/ConiferForest/props.json"]
    authored = list((repo / "projects/world_demo/shared-lib").glob("conifer*.js"))
    authored += list((repo / "projects/world_demo/objects/vegetation/conifer").glob("Conifer*.js"))
    authored += [repo / "projects/world_demo/objects/vegetation/trees/RedwoodBarkDetail.js",
                 repo / "projects/world_demo/scenes/vegetation/ConiferForest/ConiferForest.js"]
    metadata = {"controls": controls,
                "pixel_budget": args.pixel_budget,
                "executable_sha256": hashlib.sha256(editor.read_bytes()).hexdigest(),
                "source_sha256": {str(p.relative_to(repo)): hashlib.sha256(p.read_bytes()).hexdigest()
                                  for p in sorted(authored)},
                "sources": {str(p.relative_to(repo)): p.read_text() for p in sources}}
    (out / "settings.json").write_text(json.dumps(metadata, indent=2))
    with (out / "editor.log").open("wb") as log:
        process = subprocess.Popen([str(editor)], cwd=repo / "MatterEditor",
                                   env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            status = process.wait(timeout=args.timeout)
        except subprocess.TimeoutExpired:
            # Terminate only the process launched by this measurement.
            process.kill()
            process.wait()
            raise SystemExit(f"Forest measurement timed out; inspect {out / 'editor.log'}")
    if status or not (out / "perf.json").exists():
        raise SystemExit(f"Forest measurement failed ({status}); inspect {out / 'editor.log'}")
    print(out / "perf.json")


if __name__ == "__main__":
    main()
