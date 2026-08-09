#!/usr/bin/env bash
# One-editor visual/performance capture harness for atmosphere/cloud milestones.
# Usage: tools/atmosphere_cloud_shots.sh <suite> <label> <out-dir>
set -euo pipefail
SUITE="${1:?usage: atmosphere_cloud_shots.sh <suite> <label> <out-dir>}"
LABEL="${2:?usage: atmosphere_cloud_shots.sh <suite> <label> <out-dir>}"
OUT="${3:?usage: atmosphere_cloud_shots.sh <suite> <label> <out-dir>}"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
FIFO="${TMPDIR:-/tmp}/matter_atmosphere_clouds_$$.fifo"
LOG="$OUT/${LABEL}_viewer.log"
COMMANDS="$OUT/${LABEL}_commands.log"
PERF_OUTPUT="$OUT/${LABEL}_telemetry.json"
WINDOWS_COMMAND_FILE=0
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) WINDOWS_COMMAND_FILE=1 ;; esac
PERF_OUTPUT_ENV="$PERF_OUTPUT"
if [ "$WINDOWS_COMMAND_FILE" = 1 ]; then
  PERF_OUTPUT_ENV="$(cygpath -w "$PERF_OUTPUT")"
fi

send() {
  if [ "$WINDOWS_COMMAND_FILE" = 1 ]; then
    printf '%s\n' "$*" | tee -a "$COMMANDS" >> "$FIFO"
  else
    printf '%s\n' "$*" | tee -a "$COMMANDS" > "$FIFO"
  fi
}
capture() {
  local name="$1" file_stem png shot_path
  file_stem="${2:-${LABEL}_${name}}"
  png="$OUT/${file_stem}.png"
  shot_path="$png"
  if [ "$WINDOWS_COMMAND_FILE" = 1 ]; then
    shot_path="$(cygpath -w "$png")"
  fi
  rm -f "$png" "${png}.done"
  send "stats $name"
  send "shot $shot_path"
  for _ in $(seq 1 60); do [ -e "${png}.done" ] && return; sleep 1; done
  echo "ERROR: screenshot timed out: $png" >&2
  exit 1
}

HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE/../../MatterEditor"
if [ "$WINDOWS_COMMAND_FILE" = 1 ]; then
  : > "$FIFO"
else
  mkfifo "$FIFO"
fi
rm -f "$LOG" "$COMMANDS" "$PERF_OUTPUT" \
  "$OUT/${LABEL}_stats.log" "$OUT/${LABEL}_metrics.log" \
  "$OUT/${LABEL}_"*.png "$OUT/${LABEL}_"*.png.done \
  "$OUT/physical-sky_"*.png "$OUT/physical-sky_"*.png.done
: > "$COMMANDS"
PID=""
cleanup() {
  if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
    if [ "$WINDOWS_COMMAND_FILE" = 1 ]; then
      send "quit" || true
    else
      (send "quit") &
      local quit_pid=$!
      for _ in $(seq 1 5); do
        kill -0 "$quit_pid" 2>/dev/null || break
        sleep 1
      done
      kill "$quit_pid" 2>/dev/null || true
      wait "$quit_pid" 2>/dev/null || true
    fi
    for _ in $(seq 1 30); do
      kill -0 "$PID" 2>/dev/null || break
      sleep 1
    done
    if kill -0 "$PID" 2>/dev/null; then kill "$PID" 2>/dev/null || true; fi
    wait "$PID" 2>/dev/null || true
  fi
  PID=""
  rm -f "$FIFO"
}
trap cleanup EXIT INT TERM

WORLD="${MATTER_WORLD:-StreamMountain}"
FROXEL_PERF_WARMUP_SECONDS=140
PERF_WARMUP_SECONDS="${MATTER_PERF_WARMUP_SECONDS:-20}"
# The one-process froxel lane spends 25 seconds on the matrix, then settles
# and captures four representatives.  Do not let the editor's perf timer end
# the process halfway through that required proof.
if [ "$SUITE" = "froxel" ] && [ "$PERF_WARMUP_SECONDS" -lt "$FROXEL_PERF_WARMUP_SECONDS" ]; then
  PERF_WARMUP_SECONDS="$FROXEL_PERF_WARMUP_SECONDS"
fi
MATTER_WORLD="$WORLD" \
MATTER_CMD_FIFO="$FIFO" \
TMP="${TMP:?TMP must be set for the Windows editor}" \
TEMP="${TEMP:?TEMP must be set for the Windows editor}" \
MATTER_HIDE_UI="${MATTER_HIDE_UI:-1}" \
MATTER_PERF_OUTPUT="$PERF_OUTPUT_ENV" \
MATTER_PERF_WARMUP_SECONDS="$PERF_WARMUP_SECONDS" \
MATTER_PERF_SAMPLE_SECONDS="${MATTER_PERF_SAMPLE_SECONDS:-1}" \
stdbuf -oL ./build/windows/editor.exe > "$LOG" 2>&1 &
PID=$!

# Both markers matter: baking can finish before the command transport is ready.
# Native Windows polls an append-only command file; POSIX listens on the FIFO.
READY=0
for _ in $(seq 1 300); do
  if ! kill -0 "$PID" 2>/dev/null; then break; fi
  if grep -q 'viewer: bake ready' "$LOG" 2>/dev/null && \
     { grep -q 'MATTER_CMD_FIFO: listening' "$LOG" 2>/dev/null || \
       grep -q 'MATTER_CMD_FIFO: polling command file' "$LOG" 2>/dev/null; }; then
    READY=1
    break
  fi
  sleep 1
done
if [ "$READY" != 1 ]; then
  echo "ERROR: viewer did not report bake and command readiness. Log tail:" >&2
  tail -n 20 "$LOG" >&2 || true
  exit 1
fi

# Keep suite names stable; subsequent milestones add their property batches here.
case "$SUITE" in
  baseline)
    if [ "$WORLD" = StreamMountain ]; then
      # Configure before streaming creates the first drawable frame: perf
      # cannot leave WaitingForBake until instances_drawn is nonzero.
      send "set render.volumetrics.enabled true"
      send "get render.volumetrics.enabled"
      send "cam 20 760 350 0 420 0"
      # Streaming worlds announce bake readiness before their first sectors
      # publish; wait for that publish before the settled screenshot.
      for _ in $(seq 1 300); do
        grep -q 'bake-timing.*world-kind' "$LOG" 2>/dev/null && break
        sleep 1
      done
      grep -q 'bake-timing.*world-kind' "$LOG" 2>/dev/null || {
        echo "ERROR: StreamMountain sectors did not publish" >&2
        exit 1
      }
    else
      send "get render.volumetrics.enabled"
      send "cam 128 260 -40 128 0 128"
    fi
    sleep 2
    capture procedural_sky
    for _ in $(seq 1 30); do
      [ -s "$PERF_OUTPUT" ] && break
      sleep 1
    done
    [ -s "$PERF_OUTPUT" ] || {
      echo "ERROR: telemetry timed out: $PERF_OUTPUT" >&2
      exit 1
    }
    ;;
  atmosphere)
    [ "$WORLD" = StreamMountain ] || {
      echo "ERROR: atmosphere suite requires StreamMountain" >&2
      exit 2
    }
    send "set render.volumetrics.enabled true"
    send "get render.volumetrics.enabled"
    # Force the registered Current cost preset fields so persisted/user env
    # overrides cannot silently turn this baseline into an enhanced run.
    send "set render.volumetrics.froxel_xy_scale 1x"
    send "set render.volumetrics.froxel_depth_slices 128"
    send "set render.volumetrics.local_sun_march_steps 0"
    send "set render.volumetrics.local_sun_march_distance_m 250"
    send "set render.volumetrics.multiple_scattering_orders 1"
    send "set render.volumetrics.multiple_scattering_strength 0"
    send "set render.volumetrics.powder_strength 0"
    send "set render.cloud_shadows.enabled false"
    for property in \
      render.volumetrics.froxel_xy_scale \
      render.volumetrics.froxel_depth_slices \
      render.volumetrics.local_sun_march_steps \
      render.volumetrics.local_sun_march_distance_m \
      render.volumetrics.multiple_scattering_orders \
      render.volumetrics.multiple_scattering_strength \
      render.volumetrics.powder_strength \
      render.cloud_shadows.enabled \
      render.atmosphere.sea_level_y \
      render.atmosphere.rayleigh_scale \
      render.atmosphere.mie_scale \
      render.atmosphere.mie_anisotropy \
      render.atmosphere.ozone_scale \
      render.atmosphere.ground_albedo \
      render.lighting.exposure_ev \
      render.lighting.sun_multiplier \
      render.lighting.sky_multiplier \
      render.lighting.sun_tint \
      render.lighting.sky_tint \
      render.lighting.sun_azimuth_deg; do
      send "get $property"
    done
    send "cam 20 760 350 0 420 0"
    for _ in $(seq 1 300); do
      grep -q 'bake-timing.*world-kind' "$LOG" 2>/dev/null && break
      sleep 1
    done
    grep -q 'bake-timing.*world-kind' "$LOG" 2>/dev/null || {
      echo "ERROR: StreamMountain sectors did not publish" >&2
      exit 1
    }
    for elevation in 90 45 5 0 -5; do
      if [ "$elevation" = "-5" ]; then
        # Dark adaptation only for the below-horizon diagnostic: daylight
        # remains at the world baseline while nonzero twilight is legible.
        send "set render.lighting.exposure_ev 5"
      else
        send "set render.lighting.exposure_ev 0"
      fi
      send "get render.lighting.exposure_ev"
      send "set render.lighting.sun_elevation_deg $elevation"
      send "get render.lighting.sun_elevation_deg"
      sleep 3
      case "$elevation" in
        90) capture "sun_90" "physical-sky_current-cost" ;;
        *) capture "sun_${elevation}" "physical-sky_${elevation}deg" ;;
      esac
    done
    for _ in $(seq 1 30); do [ -s "$PERF_OUTPUT" ] && break; sleep 1; done
    [ -s "$PERF_OUTPUT" ] || {
      echo "ERROR: telemetry timed out: $PERF_OUTPUT" >&2
      exit 1
    }
    ;;
  froxel)
    [ "$WORLD" = StreamMountain ] || {
      echo "ERROR: froxel suite requires StreamMountain" >&2
      exit 2
    }
    send "set render.volumetrics.enabled true"
    send "get render.volumetrics.enabled"
    # Keep this resize comparison at Task 7's Current-cost lighting baseline.
    # Otherwise any inherited enhanced-cloud setting changes both the workload
    # and the image while the grid is being measured.
    send "set render.volumetrics.local_sun_march_steps 0"
    send "set render.volumetrics.local_sun_march_distance_m 250"
    send "set render.volumetrics.multiple_scattering_orders 1"
    send "set render.volumetrics.multiple_scattering_strength 0"
    send "set render.volumetrics.powder_strength 0"
    send "set render.cloud_shadows.enabled false"
    send "set render.lighting.exposure_ev 0"
    send "set render.lighting.sun_azimuth_deg -14"
    send "set render.lighting.sun_elevation_deg 90"
    for property in \
      render.volumetrics.local_sun_march_steps \
      render.volumetrics.local_sun_march_distance_m \
      render.volumetrics.multiple_scattering_orders \
      render.volumetrics.multiple_scattering_strength \
      render.volumetrics.powder_strength \
      render.cloud_shadows.enabled \
      render.lighting.exposure_ev \
      render.lighting.sun_azimuth_deg \
      render.lighting.sun_elevation_deg; do
      send "get $property"
    done
    send "cam 20 760 350 0 420 0"
    for _ in $(seq 1 300); do
      grep -q 'bake-timing.*world-kind' "$LOG" 2>/dev/null && break
      sleep 1
    done
    grep -q 'bake-timing.*world-kind' "$LOG" 2>/dev/null || {
      echo "ERROR: StreamMountain sectors did not publish" >&2
      exit 1
    }
    # Exercise every discrete pair in one process. The representative pairs
    # below are deliberately revisited and captured after their own settle.
    for xy in 0.5x 0.75x 1x 1.5x 2x; do
      for depth in 64 96 128 192 256; do
        send "set render.volumetrics.froxel_xy_scale $xy"
        send "set render.volumetrics.froxel_depth_slices $depth"
        send "get render.volumetrics.froxel_xy_scale"
        send "get render.volumetrics.froxel_depth_slices"
        sleep 1
        done
    done
    for pair in "0.5x 64 low" "1x 128 current" "1.5x 192 high" "2x 256 ultra"; do
      set -- $pair
      send "set render.volumetrics.froxel_xy_scale $1"
      send "set render.volumetrics.froxel_depth_slices $2"
      send "get render.volumetrics.froxel_xy_scale"
      send "get render.volumetrics.froxel_depth_slices"
      sleep 4
      capture "froxel_$3" "${LABEL}_$3"
    done
    # The forced 140-second lifetime is intentionally longer than the sweep;
    # wait through that timer rather than rejecting four valid captures before
    # the editor has emitted its telemetry.
    TELEMETRY_WAIT_SECONDS=$((FROXEL_PERF_WARMUP_SECONDS + 30))
    for _ in $(seq 1 "$TELEMETRY_WAIT_SECONDS"); do
      [ -s "$PERF_OUTPUT" ] && break
      sleep 1
    done
    [ -s "$PERF_OUTPUT" ] || {
      echo "ERROR: telemetry timed out: $PERF_OUTPUT" >&2
      exit 1
    }
    ;;
  cloud-lighting|cloud-shadows|final)
    echo "ERROR: suite '$SUITE' is reserved for a later milestone" >&2
    exit 2
    ;;
  *)
    echo "ERROR: unknown suite '$SUITE' (baseline, atmosphere, froxel, cloud-lighting, cloud-shadows, final)" >&2
    exit 2
    ;;
esac

cleanup
trap - EXIT INT TERM

grep '^STATS,' "$LOG" > "$OUT/${LABEL}_stats.log" || true
[ -s "$OUT/${LABEL}_stats.log" ] || {
  echo "ERROR: no positional STATS rows in $LOG" >&2
  exit 1
}
grep '"gpu_volumetrics_ms"' "$PERF_OUTPUT" > "$OUT/${LABEL}_metrics.log" || true
[ -s "$OUT/${LABEL}_metrics.log" ] || {
  echo "ERROR: no gpu_volumetrics_ms telemetry in $PERF_OUTPUT" >&2
  exit 1
}
echo "--- $LABEL: $SUITE capture and telemetry in $OUT"
