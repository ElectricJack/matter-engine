#!/usr/bin/env bash
# Scripted screenshot/metrics run: launch the viewer, drive the standard
# 5-pose camera set over the command FIFO, capture a PNG + STATS line per
# pose, then quit. The viewer ALWAYS terminates (quit + wait, kill on trap).
#
# Usage: tools/viewer_shots.sh <label> <out-dir>
#   e.g. MATTER_GPU_CULL=1 GALLIUM_DRIVER=d3d12 tools/viewer_shots.sh gpuon /tmp/ab
# Env passes through (MATTER_WORLD defaults to meadow, MATTER_GPU_CULL, ...).
# Outputs: <out-dir>/<label>_<pose>.png and <out-dir>/<label>_stats.log
set -euo pipefail
LABEL="${1:?usage: viewer_shots.sh <label> <out-dir>}"
OUT="${2:?usage: viewer_shots.sh <label> <out-dir>}"
mkdir -p "$OUT"
HERE="$(cd "$(dirname "$0")" && pwd)"
# Source shared pose set.
# shellcheck source=tools/lib_poses.sh
source "$HERE/lib_poses.sh"
cd "$HERE/../viewer"

FIFO="/tmp/matter_shots_$$.fifo"
LOG="$OUT/${LABEL}_viewer.log"
mkfifo "$FIFO"
MATTER_WORLD="${MATTER_WORLD:-meadow}" MATTER_CMD_FIFO="$FIFO" stdbuf -oL ./viewer > "$LOG" 2>&1 &
PID=$!
trap 'kill $PID 2>/dev/null || true; rm -f "$FIFO"' EXIT

# Readiness: poll the log for the FIFO-listener line instead of a fixed sleep.
# The viewer prints "MATTER_CMD_FIFO: listening" after the world has loaded.
# Cap: 180 s (cold bake can take ~40 s; allow generous margin).
READY=0
for _ in $(seq 1 180); do
    if ! kill -0 "$PID" 2>/dev/null; then break; fi
    if grep -q 'MATTER_CMD_FIFO: listening' "$LOG" 2>/dev/null; then READY=1; break; fi
    sleep 1
done
if [ "$READY" != 1 ]; then
    echo "ERROR: viewer not ready after 180s (or died). Log tail:" >&2
    tail -n 10 "$LOG" >&2
    exit 1
fi
sleep 2   # settle a few frames after readiness signal

shoot() {  # name px py pz tx ty tz
  local name="$1"; shift
  local png="$OUT/${LABEL}_${name}.png"
  rm -f "$png" "${png}.done"
  echo "cam $*" > "$FIFO"
  sleep 2                       # let LOD/batches settle at the new view
  echo "stats $name" > "$FIFO"
  echo "shot $png" > "$FIFO"
  for _ in $(seq 1 30); do [ -e "${png}.done" ] && break; sleep 1; done
  [ -e "${png}.done" ] || { echo "ERROR: shot $name timed out" >&2; exit 1; }
}

# Fire the standard 5-pose meadow set.
for pose in "${POSES_MEADOW[@]}"; do
    # shellcheck disable=SC2086
    shoot $pose
done

echo "quit" > "$FIFO"
wait "$PID" || true
trap - EXIT
rm -f "$FIFO"

grep '^STATS,' "$LOG" > "$OUT/${LABEL}_stats.log" || true
echo "--- $LABEL: shots + stats in $OUT (viewer exited)"
