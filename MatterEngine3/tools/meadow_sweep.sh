#!/usr/bin/env bash
# Meadow benchmark sweep (frame-time package, Stage 0).
# Usage: tools/meadow_sweep.sh <stage-label>   e.g. tools/meadow_sweep.sh baseline
set -euo pipefail
STAGE="${1:?usage: meadow_sweep.sh <stage-label>}"
HERE="$(cd "$(dirname "$0")" && pwd)"
# Source shared pose set.
# shellcheck source=tools/lib_poses.sh
source "$HERE/lib_poses.sh"
cd "$HERE/../../MatterViewer"

FIFO="/tmp/matter_sweep_$$.fifo"
LOG="/tmp/matter_sweep_$$.log"
mkfifo "$FIFO"
MATTER_WORLD=meadow MATTER_CMD_FIFO="$FIFO" stdbuf -oL ./viewer > "$LOG" 2>&1 &
PID=$!
trap 'kill $PID 2>/dev/null || true; rm -f "$FIFO" "$LOG"' EXIT

# Readiness: poll the log for "viewer: bake ready". A binary that never
# prints it times out at 300s (cold bake can take ~180s; allow margin).
READY=0
for _ in $(seq 1 300); do
    if ! kill -0 "$PID" 2>/dev/null; then break; fi
    if grep -q 'viewer: bake ready' "$LOG" 2>/dev/null; then READY=1; break; fi
    sleep 1
done
if [ "$READY" != 1 ]; then
    echo "ERROR: viewer not ready after 300s (or died). Log tail:" >&2
    tail -n 10 "$LOG" >&2
    exit 1
fi
sleep 3   # settle a few frames


run_cam() {  # name px py pz tx ty tz
  local name="$1"; shift
  echo "cam $*" > "$FIFO"
  sleep 2                        # let LOD/batches settle at the new view
  echo "stats $name" > "$FIFO"
  sleep 1
}

# Fire the standard 5-pose meadow set.
for pose in "${POSES_MEADOW[@]}"; do
    # shellcheck disable=SC2086
    run_cam $pose
done

echo "quit" > "$FIFO"
wait "$PID" || true

CSV="$HERE/../docs/perf/meadow_sweep.csv"
TODAY="$(date +%F)"
grep '^STATS,' "$LOG" | while IFS= read -r line; do
  echo "$TODAY,$STAGE,${line#STATS,}" >> "$CSV"
done
echo "--- appended to $CSV:"
tail -n 5 "$CSV"
