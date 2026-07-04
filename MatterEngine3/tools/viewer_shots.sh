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
cd "$HERE/../viewer"

FIFO="/tmp/matter_shots_$$.fifo"
LOG="$OUT/${LABEL}_viewer.log"
mkfifo "$FIFO"
MATTER_WORLD="${MATTER_WORLD:-meadow}" MATTER_CMD_FIFO="$FIFO" ./viewer > "$LOG" 2>&1 &
PID=$!
trap 'kill $PID 2>/dev/null || true; rm -f "$FIFO"' EXIT

sleep 25   # world bake/load + first frames

shoot() {  # name px py pz tx ty tz
  local png="$OUT/${LABEL}_$1.png"
  rm -f "$png" "$png.done"
  echo "cam $2 $3 $4 $5 $6 $7" > "$FIFO"
  sleep 2                       # let LOD/batches settle at the new view
  echo "stats $1" > "$FIFO"
  echo "shot $png" > "$FIFO"
  for _ in $(seq 1 30); do [ -e "$png.done" ] && break; sleep 1; done
  [ -e "$png.done" ] || { echo "ERROR: shot $1 timed out" >&2; exit 1; }
}
# Fixed camera set (matches meadow_sweep.sh):
shoot aerial    128 260 -40   128 0 128
shoot corner      8   2   8    60 1  60
shoot midfield   40   6  40   128 2 128
shoot far         4   3   4   250 0 250
shoot empty     -40   5 -40  -200 5 -200

echo "quit" > "$FIFO"
wait "$PID" || true
trap - EXIT
rm -f "$FIFO"

grep '^STATS,' "$LOG" > "$OUT/${LABEL}_stats.log" || true
echo "--- $LABEL: shots + stats in $OUT (viewer exited)"
