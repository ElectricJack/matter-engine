#!/usr/bin/env bash
# Meadow benchmark sweep (frame-time package, Stage 0).
# Usage: tools/meadow_sweep.sh <stage-label>   e.g. tools/meadow_sweep.sh baseline
set -euo pipefail
STAGE="${1:?usage: meadow_sweep.sh <stage-label>}"
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE/../viewer"

FIFO="/tmp/matter_sweep_$$.fifo"
LOG="/tmp/matter_sweep_$$.log"
mkfifo "$FIFO"
MATTER_WORLD=meadow MATTER_CMD_FIFO="$FIFO" ./viewer > "$LOG" 2>&1 &
PID=$!
trap 'kill $PID 2>/dev/null || true; rm -f "$FIFO" "$LOG"' EXIT

sleep 55   # world bake ~40 s on cold cache + margin; aerial must land in steady-state

run_cam() {  # name px py pz tx ty tz
  echo "cam $2 $3 $4 $5 $6 $7" > "$FIFO"
  sleep 2                        # let LOD/batches settle at the new view
  echo "stats $1" > "$FIFO"
  sleep 1
}
# Fixed camera set from the spec (meadow world spans 0..256):
run_cam aerial    128 260 -40   128 0 128    # whole scene in frustum
run_cam corner      8   2   8    60 1  60    # in-crowd ground view
run_cam midfield   40   6  40   128 2 128    # mid-distance, grass tri-share peak
run_cam far         4   3   4   250 0 250    # far ground view across the world
run_cam empty     -40   5 -40  -200 5 -200   # outside, looking away: fixed CPU cost

echo "quit" > "$FIFO"
wait "$PID" || true

CSV="$HERE/../docs/perf/meadow_sweep.csv"
TODAY="$(date +%F)"
grep '^STATS,' "$LOG" | while IFS= read -r line; do
  echo "$TODAY,$STAGE,${line#STATS,}" >> "$CSV"
done
echo "--- appended to $CSV:"
tail -n 5 "$CSV"
