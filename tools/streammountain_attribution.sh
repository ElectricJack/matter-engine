#!/usr/bin/env bash
# Matched StreamMountain perf captures: POM reference, chart_only, work, and
# POM disabled. Same warmup, same sample window, same camera (the world's
# default), same resolution, sequential on one GPU. Output, per run:
# <out>/<run>.json (MATTER_PERF_OUTPUT), .trace.json (MATTER_PROFILE_TRACE),
# .log, .commands.txt and the nvidia-smi logs; then <out>/attribution.md from
# tools/frame_attribution.py, <out>/hitches.md from its --hitches summary
# (GPU total median/p99/max, frames over 100 ms and 1 s, frame-interval
# histogram, peak VRAM) and <out>/cpu_zones.md from the traces. A run is named
# after its variant, or <variant>_r<N> when RUNS > 1.
#
# POM toggle: POM is off by default (TilesetPomSettings::enabled, 2026-09-28)
# and StreamMountain also saves render.pom.enabled=false in
# scenes/streaming/StreamMountain/props.json. No environment variable turns
# POM on or off, so the three MATTER_GBUFFER_POM_PATH variants set it true
# through the command FIFO after bake.finished; pom_off sends no `set`, so it
# measures the world as it ships. `set` on a World prop is not persisted (world
# props save only on an explicit Save or a world switch); each run fails unless
# perf.json's pom_enabled field matches what was requested.
#
# The clear-ridge POM-off baseline (docs/vg-vt-work-queue-2026-09-19.md) is
#   VARIANTS=pom_off RUNS=3 WARMUP=45  tools/streammountain_attribution.sh C:/tmp/<dir>
#   VARIANTS=pom_off RUNS=3 WARMUP=300 tools/streammountain_attribution.sh C:/tmp/<dir>
#
# Presentation is uncapped (IMMEDIATE, frame limit 0) with a visible window:
# the 2026-09-17 terrain-framerate runs found that a hidden window and FIFO
# presentation throttle cadence and GPU clocks. Each run self-terminates after
# sampling (`perf: wrote N frames`); RUN_TIMEOUT bounds a run that never
# settles, and the trap stops only this checkout's editor.exe.
#
# Usage: tools/streammountain_attribution.sh C:/tmp/attr
# Overrides: WARMUP (45) SAMPLE (20) WIDTH (1920) HEIGHT (1080) RUNS (1)
#            RUN_TIMEOUT (1800 s) VARIANTS ("pom_reference pom_chart_only pom_work pom_off")
#            PAGED_TERRAIN (0; 1 opts into the StreamMountain terrain geometry profile)
#            PAGED_CACHE_ONLY (0; 1 forbids missing terrain-page compilation in that profile)
#            PAGED_GPU_MB (3072; geometry page GPU budget, 1..4096 MiB)
#            PAGED_VT_MB (2048; physical VT pool budget, 1..16384 MiB)
#            GBUFFER_WORKLOAD (0; 1 logs optional raster pipeline counters)
#            LIGHTING_DETAIL (0; 1 splits GI dispatches at unchanged resolutions)
#            GI_SPECIALIZE (auto; 0 selects the original GI shader, 1 separate compiled lanes)
#            EDITOR_NAME (editor.exe; executable in MatterEditor/build/windows-msvc)
#            Diagnostic variants: geometry, geometry_cutout, no_vt (POM off; images
#            differ from pom_off and their timings are differential evidence).
set -euo pipefail
OUT=${1:?usage: streammountain_attribution.sh <out-dir-windows-path e.g. C:/tmp/attr>}
case "$OUT" in C:/*) ;; *) echo "out dir must be a space-free C:/ path" >&2; exit 2 ;; esac
WARM=${WARMUP:-45}; SAMPLE=${SAMPLE:-20}; WIDTH=${WIDTH:-1920}; HEIGHT=${HEIGHT:-1080}
RUN_TIMEOUT=${RUN_TIMEOUT:-1800}; RUNS=${RUNS:-1}
PIPELINE_STATS=${PIPELINE_STATS:-0}
stats_env=()
if [ "${LIGHTING_DETAIL:-0}" = 1 ]; then stats_env+=(MATTER_GPU_LIGHTING_DETAIL_TIMERS=1); fi
case "${GI_SPECIALIZE:-auto}" in
  auto) ;;
  0|1) stats_env+=(MATTER_RT_GI_SPECIALIZE="$GI_SPECIALIZE") ;;
  *) echo "GI_SPECIALIZE must be auto, 0 or 1" >&2; exit 2 ;;
esac
if [ "${GBUFFER_WORKLOAD:-0}" = 1 ]; then stats_env+=(MATTER_GBUFFER_WORKLOAD=1); fi
if [ "$PIPELINE_STATS" = 1 ]; then stats_env+=(MATTER_VK_PIPELINE_STATS=1); fi
paged_env=()
if [ "${PAGED_TERRAIN:-0}" = 1 ]; then
  paged_gpu_mb=${PAGED_GPU_MB:-3072}
  paged_vt_mb=${PAGED_VT_MB:-2048}
  case "$paged_gpu_mb" in ''|*[!0-9]*) echo "PAGED_GPU_MB must be 1..4096" >&2; exit 2 ;; esac
  if [ "$paged_gpu_mb" -lt 1 ] || [ "$paged_gpu_mb" -gt 4096 ]; then
    echo "PAGED_GPU_MB must be 1..4096" >&2; exit 2
  fi
  case "$paged_vt_mb" in ''|*[!0-9]*) echo "PAGED_VT_MB must be 1..16384" >&2; exit 2 ;; esac
  if [ "$paged_vt_mb" -lt 1 ] || [ "$paged_vt_mb" -gt 16384 ]; then
    echo "PAGED_VT_MB must be 1..16384" >&2; exit 2
  fi
  paged_env=(MATTER_GEOMETRY_TERRAIN=1 MATTER_GEOMETRY_PAGES=1
    MATTER_GEOMETRY_MODULE=MountainDetailRock MATTER_GEOMETRY_MIN_TRIANGLES=16384
    MATTER_GEOMETRY_ROOT_MB=1024 MATTER_GEOMETRY_CPU_MB=1024 MATTER_GEOMETRY_GPU_MB="$paged_gpu_mb"
    MATTER_VT_POOL_MB="$paged_vt_mb" MATTER_GEOMETRY_PAGES_PROFILE=1)
  if [ "${PAGED_CACHE_ONLY:-0}" = 1 ]; then paged_env+=(MATTER_GEOMETRY_CACHE_ONLY=1); fi
fi
case "$RUNS" in ''|*[!0-9]*|0) echo "RUNS must be a positive integer" >&2; exit 2 ;; esac
VARIANTS=${VARIANTS:-"pom_reference pom_chart_only pom_work pom_off"}
WOUT="/mnt/c/${OUT#C:/}"
REPO=$(cd "$(dirname "$0")/.." && pwd)
PS=/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe
EDITOR_NAME=${EDITOR_NAME:-editor.exe}
case "$EDITOR_NAME" in *[!a-zA-Z0-9_.-]*|''|.*) echo "invalid EDITOR_NAME" >&2; exit 2 ;; esac
EDITOR_WIN=$(wslpath -w "$REPO/MatterEditor/build/windows-msvc/$EDITOR_NAME")
# Launch rule 1 (docs/agent/control-surface.md): a native exe needs TMP/TEMP.
WTEMP=$(cd /mnt/c && /mnt/c/Windows/System32/cmd.exe /c 'echo %LOCALAPPDATA%\Temp' | tr -d '\r')
# Inherited MATTER_* settings would silently change one variant or all four.
for v in $(compgen -e | grep '^MATTER_' || true); do unset "$v"; done

stop_own_editor() {
  "$PS" -NoProfile -Command "Get-CimInstance Win32_Process -Filter \"Name='$EDITOR_NAME'\" |
    Where-Object { \$_.ExecutablePath -eq '$EDITOR_WIN' } |
    ForEach-Object { Stop-Process -Id \$_.ProcessId -Force }" >/dev/null 2>&1 || true
}
smi_pid=""
trap 'stop_own_editor; [ -n "$smi_pid" ] && kill "$smi_pid" 2>/dev/null' EXIT

SMI=/mnt/c/Windows/System32/nvidia-smi.exe
# The GPU is shared: wait until nothing else holds VRAM (GPU_IDLE_MIB, default
# 2048) before each launch, and log utilization/memory every 5 s during it.
wait_for_idle_gpu() {
  local used
  while :; do
    used=$("$SMI" --query-gpu=memory.used --format=csv,noheader,nounits | tr -d ' \r')
    [ "$used" -lt "${GPU_IDLE_MIB:-2048}" ] && return 0
    echo "waiting: GPU has $used MiB in use" >&2; sleep 30
  done
}

run() { # name, pom (true|false), extra env...
  local name=$1 pom=$2; shift 2
  # World props (render.pom among them) are applied on bake.finished, so a
  # `set` dispatched earlier is overwritten; wait for the event first. POM off
  # is the default, so pom=false sends no `set` and checks the shipped state.
  printf 'wait_event bake.finished 3600\nwait_frames 2\n' > "$WOUT/$name.commands.txt"
  [ "$pom" = true ] && printf 'set render.pom.enabled true\n' >> "$WOUT/$name.commands.txt"
  wait_for_idle_gpu
  { date -Is; "$SMI" --query-gpu=name,driver_version,utilization.gpu,memory.used --format=csv,noheader; } \
      > "$WOUT/$name.gpu_before.txt" 2>&1 || true
  echo "== $name (pom=$pom $*)"
  "$SMI" --query-gpu=timestamp,utilization.gpu,memory.used --format=csv -l 5 > "$WOUT/$name.gpu_during.csv" 2>&1 &
  smi_pid=$!
  local rc=0 start; start=$(date +%s.%N)
  env WSLENV=MATTER_RT_GI_SPECIALIZE:MATTER_GPU_LIGHTING_DETAIL_TIMERS:MATTER_GBUFFER_WORKLOAD:MATTER_WORLD:MATTER_PERF_OUTPUT:MATTER_PERF_WARMUP_SECONDS:MATTER_PERF_SAMPLE_SECONDS:MATTER_PROFILE_TRACE:MATTER_GBUFFER_POM_PATH:MATTER_GBUFFER_PROFILE_MODE:MATTER_VK_PIPELINE_STATS:MATTER_CMD_FIFO:MATTER_HIDE_WINDOW:MATTER_HIDE_UI:MATTER_WINDOW_WIDTH:MATTER_WINDOW_HEIGHT:MATTER_PRESENT_MODE:MATTER_FRAME_LIMIT:MATTER_GEOMETRY_TERRAIN:MATTER_GEOMETRY_PAGES:MATTER_GEOMETRY_MODULE:MATTER_GEOMETRY_MIN_TRIANGLES:MATTER_GEOMETRY_ROOT_MB:MATTER_GEOMETRY_CPU_MB:MATTER_GEOMETRY_GPU_MB:MATTER_VT_POOL_MB:MATTER_GEOMETRY_PAGES_PROFILE:MATTER_GEOMETRY_CACHE_ONLY:TMP:TEMP \
      TMP="$WTEMP" TEMP="$WTEMP" \
      MATTER_WORLD=StreamMountain MATTER_PERF_OUTPUT="$OUT/$name.json" \
      MATTER_PERF_WARMUP_SECONDS="$WARM" MATTER_PERF_SAMPLE_SECONDS="$SAMPLE" \
      MATTER_PROFILE_TRACE="$OUT/$name.trace.json" MATTER_CMD_FIFO="$OUT/$name.commands.txt" \
      MATTER_HIDE_WINDOW=0 MATTER_HIDE_UI=1 MATTER_WINDOW_WIDTH="$WIDTH" MATTER_WINDOW_HEIGHT="$HEIGHT" \
      MATTER_PRESENT_MODE=immediate MATTER_FRAME_LIMIT=0 "${stats_env[@]}" "${paged_env[@]}" "$@" \
      timeout --kill-after=30 "$RUN_TIMEOUT" "./build/windows-msvc/$EDITOR_NAME" \
      > "$WOUT/$name.log" 2>&1 || rc=$?
  stop_own_editor
  kill "$smi_pid" 2>/dev/null || true; smi_pid=""
  # Launch-to-exit wall time; minus warmup + sample it bounds load + settle.
  echo "elapsed_seconds $(echo "$(date +%s.%N) - $start" | bc)" > "$WOUT/$name.wall.txt"
  if [ "$rc" -ne 0 ] || ! grep -q '^perf: wrote' "$WOUT/$name.log"; then
    echo "$name failed (exit $rc); see $WOUT/$name.log" >&2; return 1
  fi
  grep '^perf: wrote' "$WOUT/$name.log"
  python3 -c 'import json, sys; sys.exit(json.load(open(sys.argv[1]))["pom_enabled"] != (sys.argv[2] == "true"))' \
      "$WOUT/$name.json" "$pom" || { echo "$name sampled pom_enabled != $pom" >&2; return 1; }
  case "$name" in
    geometry_cutout*) expected=geometry_cutout ;;
    geometry*) expected=geometry ;;
    no_vt*) expected=no_vt ;;
    *) expected=full ;;
  esac
  python3 -c 'import json, sys; sys.exit(json.load(open(sys.argv[1])).get("gbuffer_profile_mode", "full") != sys.argv[2])' \
      "$WOUT/$name.json" "$expected" || { echo "$name sampled unexpected G-buffer profile mode" >&2; return 1; }
}

mkdir -p "$WOUT"
cd "$REPO/MatterEditor"
git -C "$REPO" rev-parse HEAD > "$WOUT/git_sha.txt"
sha256sum "./build/windows-msvc/$EDITOR_NAME" > "$WOUT/editor_sha256.txt"
jsons=()
for variant in $VARIANTS; do
  case "$variant" in
    pom_reference|pom_chart_only|pom_work|pom_off|geometry|geometry_cutout|no_vt) ;;
    *) echo "unknown variant $variant" >&2; exit 2 ;;
  esac
done
for variant in $VARIANTS; do
  for i in $(seq 1 "$RUNS"); do
    name=$variant; [ "$RUNS" -gt 1 ] && name=${variant}_r$i
    case "$variant" in
      pom_reference)  run "$name" true  MATTER_GBUFFER_POM_PATH=reference ;;
      pom_chart_only) run "$name" true  MATTER_GBUFFER_POM_PATH=chart_only ;;
      pom_work)       run "$name" true  MATTER_GBUFFER_POM_PATH=work ;;
      pom_off)        run "$name" false ;;
      geometry)       run "$name" false MATTER_GBUFFER_PROFILE_MODE=geometry ;;
      geometry_cutout) run "$name" false MATTER_GBUFFER_PROFILE_MODE=geometry_cutout ;;
      no_vt)          run "$name" false MATTER_GBUFFER_PROFILE_MODE=no_vt ;;
    esac
    jsons+=("$WOUT/$name.json")
  done
done
python3 "$REPO/tools/frame_attribution.py" "${jsons[@]}" --out "$WOUT/attribution.md"
echo "wrote $WOUT/attribution.md"
python3 "$REPO/tools/frame_attribution.py" "${jsons[@]}" --hitches --out "$WOUT/hitches.md"
echo "wrote $WOUT/hitches.md"

# CPU zones from the Chrome traces. ProfileLib writes each frame as its X zone
# events, then the frame_ms counter, then that frame's named counters, so file
# order recovers per-frame values; a zone absent from a frame cost 0 there.
# The trace is the ring's tail at exit (up to 512 frames, load frames
# included), so only its last `frames` records -- the perf sampling window
# from the matching perf.json -- are summarized.
python3 - "${jsons[@]/%.json/.trace.json}" > "$WOUT/cpu_zones.md" <<'PY'
import json, math, pathlib, sys
ZONES = ["resolve.sector_lod", "resolve.emit", "instance_cache.match", "cull.vt_demand", "rt.rung_select",
         "rt.tlas_hash", "geometry.update", "geometry.scene_completion", "geometry.scene_assembly"]
COUNTERS = ["instances.sector_lod_scanned", "instances.rt_scanned", "instances.vt_scanned"]
def frames(path):
    out, pending, lanes, window = [], {}, {}, json.loads(pathlib.Path(path.replace(".trace.json", ".json")).read_text())["frames"]
    for e in json.loads(pathlib.Path(path).read_text())["traceEvents"]:
        if e["ph"] == "X":
            pending[e["name"]] = pending.get(e["name"], 0.0) + e["dur"] / 1000.0; lanes[e["name"]] = e["tid"]
        elif e["ph"] == "C" and e["name"] == "frame_ms":
            out.append({"wall": e["args"]["ms"], "z": pending, "c": {}}); pending = {}
        elif e["ph"] == "C" and out: out[-1]["c"][e["name"]] = e["args"]["n"]
    return out[-window:], lanes
def p95(v): s = sorted(v); return s[max(0, math.ceil(0.95 * len(s)) - 1)]
def cell(v, seen, fmt): return f"{sum(v) / len(v):{fmt}} / {p95(v):{fmt}}" if seen and v else "absent"
runs = [(pathlib.Path(p).name.split(".")[0], *frames(p)) for p in sys.argv[1:]]
print("| per-frame mean / p95 | " + " | ".join(n for n, _, _ in runs) + " |\n|" + "---|" * (len(runs) + 1))
print("| frames (sampling window) | " + " | ".join(str(len(f)) for _, f, _ in runs) + " |")
print("| frame wall ms | " + " | ".join(cell([x["wall"] for x in f], True, ".2f") for _, f, _ in runs) + " |")
for z in ZONES:
    print(f"| {z} ms | " + " | ".join(cell([x["z"].get(z, 0.0) for x in f], z in l, ".3f") for _, f, l in runs) + " |")
for c in COUNTERS:
    print(f"| {c} | " + " | ".join(cell([x["c"].get(c, 0) for x in f], any(c in x["c"] for x in f), ".0f") for _, f, _ in runs) + " |")
name, f, lanes = runs[0]
top = sorted((z for z in lanes if lanes[z] == 1), key=lambda z: -sum(x["z"].get(z, 0.0) for x in f))[:15]
print(f"\nTop render-lane zones in {name} (nested zones overlap; do not sum):\n\n| zone | mean ms | p95 ms |\n|---|---|---|")
for z in top:
    v = [x["z"].get(z, 0.0) for x in f]; print(f"| {z} | {sum(v) / len(v):.3f} | {p95(v):.3f} |")
PY
echo "wrote $WOUT/cpu_zones.md"
