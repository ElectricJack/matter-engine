#!/usr/bin/env python3
"""Run one visible, fixed-camera castle lighting measurement with native Python.

The editor owns warmup/sampling and writes its perf JSON. This wrapper records
the exact executable/settings and captures a screenshot during warmup. It never
closes unrelated editor processes. Run cases sequentially on the same GPU.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time


CAMERAS = {
    "hall": "18.995929,1.650000,0.925000,24.462178,2.200000,-1.307180",
    "gold": "25.311338,1.650000,15.194475,23.403262,1.800000,19.031368",
    "exterior": "-15,24,43,16.062178,7,7.674546",
}


def show_editor_window(pid, state=None, reveal=True):
    """Verify visibility even for older binaries whose perf mode forces hiding."""
    user32 = ctypes.windll.user32
    callback_type = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    user32.GetWindowThreadProcessId.argtypes = (ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong))
    user32.GetClassNameW.argtypes = (ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_int)
    user32.ShowWindow.argtypes = (ctypes.c_void_p, ctypes.c_int)
    user32.IsWindowVisible.argtypes = (ctypes.c_void_p,)
    user32.IsIconic.argtypes = (ctypes.c_void_p,)
    user32.GetForegroundWindow.restype = ctypes.c_void_p
    visible = False

    @callback_type
    def visit(window, _):
        nonlocal visible
        owner = ctypes.c_ulong()
        user32.GetWindowThreadProcessId(window, ctypes.byref(owner))
        window_class = ctypes.create_unicode_buffer(256)
        user32.GetClassNameW(window, window_class, len(window_class))
        # CUDA/D3D and GLFW create titled helper windows too. Only the real
        # renderer window proves this is a visible graphics measurement.
        if owner.value == pid and window_class.value == "GLFW30":
            if reveal and not user32.IsWindowVisible(window):
                user32.ShowWindow(window, 5)
            visible = bool(user32.IsWindowVisible(window)) or visible
            if state is not None:
                state.update(visible=bool(user32.IsWindowVisible(window)),
                             minimized=bool(user32.IsIconic(window)),
                             foreground=window == user32.GetForegroundWindow())
        return True

    user32.EnumWindows(visit, 0)
    return visible


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--editor", required=True, type=Path)
    parser.add_argument("--out-dir", required=True, type=Path)
    parser.add_argument("--camera", choices=CAMERAS, default="hall")
    parser.add_argument("--path", choices=("raster", "native_rt"), default="native_rt")
    parser.add_argument("--dlss-mode", choices=("native", "quality", "balanced", "performance"),
                        default="native", help="Requested and verified active reconstruction mode")
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--gi", choices=("on", "off"), default="on")
    parser.add_argument("--vsync", choices=(0, 1), type=int,
                        help="Explicit presentation policy; 0 requests MAILBOX/IMMEDIATE, omit to retain the default")
    parser.add_argument("--present-mode", choices=("auto", "fifo", "mailbox", "immediate"),
                        default="auto", help="Explicit Vulkan presentation mode; auto follows vsync")
    parser.add_argument("--diffuse-multiplier", type=float, default=1.0,
                        help="Diffuse bounce strength; zero skips diffuse rays while retaining reflections/glass")
    parser.add_argument("--gi-trace-scale", type=float,
                        help="Diffuse GI width/height scale in 0.125..1; omit for compatibility with older editors")
    parser.add_argument("--reflection-trace-scale", type=float,
                        help="Independent reflection/refraction scale in 0.125..1; omit for older editors")
    parser.add_argument("--area-samples", choices=(0, 1, 4), type=int, default=4,
                        help="Primary visibility samples: 0 selects adaptive, 1 or 4 fixed")
    parser.add_argument("--material-footprint", choices=(0, 1), type=int, default=1,
                        help="Select finished-surface detail for DLSS output pixels")
    parser.add_argument("--lighting-detail-timers", choices=(0, 1), type=int, default=0,
                        help="Enable HDR, split GI and primary light-cull timing zones")
    parser.add_argument("--primary-light-culling", choices=(0, 1), type=int, default=0,
                        help="Enable conservative GPU screen-tile primary light lists")
    parser.add_argument("--primary-only", choices=(0, 1), type=int, default=0,
                        help="Isolate fixed primary shading without requiring light culling")
    parser.add_argument("--primary-cull-audit", choices=(0, 1), type=int, default=0,
                        help="Diagnostic rejected-light audit; leave off for performance")
    parser.add_argument("--visibility-counters", choices=(0, 1), type=int, default=0)
    parser.add_argument("--detail-mode", choices=(0, 1, 2), type=int, default=0)
    parser.add_argument("--keep-selection", choices=(0, 1), type=int, default=0)
    parser.add_argument("--secondary-light-sampling", choices=(0, 1), type=int, default=0)
    parser.add_argument("--capture-frames", type=int, default=8,
                        help="Frames after applying settings before the screenshot")
    parser.add_argument("--warmup", type=float, default=15)
    parser.add_argument("--sample", type=float, default=20)
    parser.add_argument("--frame-timings", choices=(0, 1), type=int, default=0,
                        help="Capture bounded raw CPU frame/wait rows in perf JSON")
    parser.add_argument("--frame-limit", type=int, default=0,
                        help="Explicit CPU frame rate limit in 0..360; zero is uncapped")
    parser.add_argument("--validation", choices=(0, 1), type=int, default=0,
                        help="Enable Vulkan validation for functional checks")
    parser.add_argument("--presentmon", type=Path,
                        help="Optional native PresentMon console executable for display timing")
    parser.add_argument("--timeout", type=float, default=420)
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("Use native Windows Python (py -3), not WSL Python")
    if args.warmup < 5 or args.sample <= 0 or args.timeout <= 0 or args.capture_frames < 1:
        parser.error("warmup must be >=5; sample, timeout and capture-frames must be positive")
    if args.width < 1 or args.height < 1:
        parser.error("width and height must be positive")
    if not 0 <= args.frame_limit <= 360:
        parser.error("frame-limit must be in 0..360")
    if not 0.0 <= args.diffuse_multiplier <= 4.0:
        parser.error("diffuse-multiplier must be finite and in 0..4")
    if args.gi_trace_scale is not None and not 0.125 <= args.gi_trace_scale <= 1.0:
        parser.error("gi-trace-scale must be finite and in 0.125..1")
    if args.reflection_trace_scale is not None and not 0.125 <= args.reflection_trace_scale <= 1.0:
        parser.error("reflection-trace-scale must be finite and in 0.125..1")
    repo = Path(__file__).resolve().parent.parent
    editor = args.editor.resolve(strict=True)
    presentmon = args.presentmon.resolve(strict=True) if args.presentmon else None
    out = args.out_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    if (out / "metadata.json").exists():
        parser.error("Use a fresh output directory to preserve previous measurements")
    commands = out / "commands.txt"
    commands.write_text("", encoding="utf-8")
    env = {key: value for key, value in os.environ.items()
           if not key.upper().startswith("MATTER_")}
    controls = {
        "MATTER_WORLD": "CastleUpgraded",
        "MATTER_HIDE_WINDOW": "0",
        "MATTER_HIDE_UI": "1",
        "MATTER_WINDOW_WIDTH": str(args.width),
        "MATTER_WINDOW_HEIGHT": str(args.height),
        "MATTER_IMPOSTOR": "0",
        "MATTER_CAM": CAMERAS[args.camera],
        "MATTER_CMD_FIFO": commands.as_posix(),
        "MATTER_PERF_OUTPUT": (out / "perf.json").as_posix(),
        "MATTER_PERF_WARMUP_SECONDS": str(args.warmup),
        "MATTER_PERF_SAMPLE_SECONDS": str(args.sample),
        "MATTER_FRAME_TIMINGS": str(args.frame_timings),
        "MATTER_PRESENT_MODE": args.present_mode,
        "MATTER_FRAME_LIMIT": str(args.frame_limit),
        "MATTER_RT_LOCAL_PRIMARY_BUDGET": "0",
        "MATTER_RT_LOCAL_SECONDARY_BUDGET": "0",
        "MATTER_RT_LOCAL_PRIMARY_SAMPLES": str(args.area_samples),
        "MATTER_DLSS_MATERIAL_FOOTPRINT": str(args.material_footprint),
        "MATTER_GPU_LIGHTING_DETAIL_TIMERS": str(args.lighting_detail_timers),
        "MATTER_PRIMARY_LIGHT_CULLING": str(args.primary_light_culling),
        "MATTER_RT_PRIMARY_ONLY": str(args.primary_only),
        "MATTER_PRIMARY_LIGHT_CULL_AUDIT": str(args.primary_cull_audit),
        "MATTER_RT_VISIBILITY_COUNTERS": str(args.visibility_counters),
        "MATTER_RT_SURFACE_DETAIL_MODE": str(args.detail_mode),
        "MATTER_RT_KEEP_LOCAL_SELECTION": str(args.keep_selection),
        "MATTER_RT_SECONDARY_LIGHT_SAMPLING": str(args.secondary_light_sampling),
    }
    # The editor's validation switch is presence-based: even "0" enables it.
    if args.validation:
        controls["MATTER_VK_VALIDATION"] = "1"
    env.update(controls)
    if args.vsync is not None:
        controls["MATTER_VSYNC"] = str(args.vsync)
        env["MATTER_VSYNC"] = str(args.vsync)
    env["TMP"] = env["TEMP"] = str(Path(env["LOCALAPPDATA"]) / "Temp")
    exterior = args.camera == "exterior"
    timeline = [
        f"dlss {args.dlss_mode}",
        "set render.pom.enabled true",
        "set render.lighting.exposure_ev -1.5",
        f"set render.lighting.sun_multiplier {1 if exterior else 0.35}",
        f"set render.lighting.sky_multiplier {1 if exterior else 0.45}",
        f"set render.lighting.sky_irradiance_multiplier {1 if exterior else 0.45}",
        f"set render.gi.enabled {'true' if args.gi == 'on' else 'false'}",
        f"set render.gi.diffuse_multiplier {args.diffuse_multiplier}",
        *([f"set render.gi.trace_scale {args.gi_trace_scale}"]
          if args.gi_trace_scale is not None else []),
        *([f"set render.gi.reflection_trace_scale {args.reflection_trace_scale}"]
          if args.reflection_trace_scale is not None else []),
        f"render_path {args.path}",
        f"wait_frames {args.capture_frames}",
        f"shot {out.as_posix()}/view.png",
    ]
    with editor.open("rb") as executable:
        executable_digest = hashlib.file_digest(executable, "sha256").hexdigest()
    metadata = {
        "editor": str(editor),
        "editor_sha256": executable_digest,
        "camera": args.camera,
        "controls": controls,
        "timeline": timeline,
        "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "verified_window_class": "GLFW30",
    }
    started = time.monotonic()
    display_proc = None
    display_log = None
    log_path = out / "log.txt"
    print(f"Visible castle measurement: {out}", flush=True)
    with log_path.open("w", encoding="utf-8") as log:
        proc = subprocess.Popen([str(editor)], cwd=repo / "MatterEditor", env=env,
                                stdout=log, stderr=subprocess.STDOUT)
        metadata["pid"] = proc.pid
        sent = False
        visible = False
        window_samples = dict(observations=0, foreground=0, minimized=0, visible=0)
        while proc.poll() is None:
            state = {}
            visible_now = show_editor_window(proc.pid, state, reveal=not visible)
            if not visible:
                visible = visible_now
                if visible:
                    metadata["visible_seconds"] = time.monotonic() - started
                    print("Editor window visibility verified", flush=True)
            if visible and not sent and "viewer: bake ready" in log_path.read_text(errors="replace"):
                with commands.open("a", encoding="utf-8") as command_file:
                    command_file.write("\n".join(timeline) + "\n")
                sent = True
                metadata["ready_seconds"] = time.monotonic() - started
                print(f"Castle ready after {metadata['ready_seconds']:.2f}s", flush=True)
            if sent and state:
                window_samples["observations"] += 1
                for key in ("foreground", "minimized", "visible"):
                    window_samples[key] += int(state[key])
            if presentmon and sent and display_proc is None and "perf: sampling" in log_path.read_text(errors="replace"):
                display_log = (out / "presentmon.log").open("w", encoding="utf-8")
                display_command = [str(presentmon), "--process_id", str(proc.pid),
                    "--output_file", str(out / "presentmon.csv"), "--no_console_stats",
                    "--no_track_input", "--session_name", f"MatterPacing-{proc.pid}",
                    "--timed", str(int(args.sample) + 3), "--terminate_after_timed",
                    "--terminate_on_proc_exit"]
                metadata["presentmon_command"] = display_command
                display_proc = subprocess.Popen(display_command, stdout=display_log,
                                                stderr=subprocess.STDOUT)
            if time.monotonic() - started > args.timeout:
                proc.kill()
                metadata["timed_out"] = True
                break
            time.sleep(0.25)
        metadata["exit_code"] = proc.wait()
    if display_proc is not None:
        try:
            metadata["presentmon_exit_code"] = display_proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            display_proc.kill()
            metadata["presentmon_exit_code"] = display_proc.wait()
            metadata["presentmon_timed_out"] = True
        display_log.close()
        display_csv = out / "presentmon.csv"
        metadata["presentmon_csv_created"] = display_csv.exists()
        if not display_csv.exists():
            print("PresentMon produced no display samples; this run only measures application/GPU timing", flush=True)
    metadata["elapsed_seconds"] = time.monotonic() - started
    metadata["commands_sent"] = sent
    metadata["window_visibility_verified"] = visible
    metadata["window_state_polls_after_commands"] = window_samples
    (out / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    perf_path = out / "perf.json"
    if metadata["exit_code"] or not sent or not visible or not perf_path.exists():
        print(f"Measurement failed; inspect {log_path}", file=sys.stderr)
        return 1
    result = json.loads(perf_path.read_text(encoding="utf-8"))
    summary = {key: result.get(key) for key in
               ("median_frame_ms", "p95_frame_ms", "median_fps", "gpu_total_ms",
                "gpu_rt_local_direct_ms", "gpu_rt_gi_ms", "gpu_dlss_ms", "validation_errors",
                "gpu_hdr_lighting_ms", "gpu_rt_gi_diffuse_ms", "gpu_rt_gi_reflection_transmission_ms",
                "gpu_primary_light_cull_ms",
                "selected_dlss_mode", "active_dlss_mode", "dlss_internal_width",
                "dlss_internal_height", "dlss_output_width", "dlss_output_height")}
    passes = result.get("gpu_pass_statistics", {}).get("passes", {})
    summary["raw_gpu_passes"] = {key: passes[key] for key in
                                 ("total", "rt_local_direct", "rt_gi", "gbuffer", "denoise", "dlss",
                                  "hdr_lighting", "rt_gi_diffuse", "rt_gi_reflection_transmission",
                                  "primary_light_cull")
                                 if key in passes}
    summary["full_result"] = str(perf_path)
    summary["present_cadence_statistics"] = result.get("present_cadence_statistics")
    print(json.dumps(summary, indent=2), flush=True)
    for key in ("selected_dlss_mode", "active_dlss_mode"):
        if str(result.get(key, "")).casefold() != args.dlss_mode:
            print(f"{key} is {result.get(key)!r}, requested {args.dlss_mode}; "
                  "do not compare a silent fallback", file=sys.stderr)
            return 1
    if (result.get("dlss_output_width"), result.get("dlss_output_height")) != (args.width, args.height):
        print("Actual output dimensions differ from the requested comparison", file=sys.stderr)
        return 1
    if not (out / "view.png.done").exists():
        print("Warmup screenshot did not complete", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
