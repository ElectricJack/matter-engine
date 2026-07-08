#!/usr/bin/env bash
# Stage-4 stress-forest scale sweep (GPU instancing/culling package).
# For each StressForest world (50k/100k/200k/500k trees over 2 km x 2 km),
# launch the viewer on the GPU cull path (HiZ on by default), run the fixed
# 5-camera stats set, and append rows to docs/perf/stress_sweep.csv:
#   date,stage,world,camera,frame_ms,resolve_ms,build_ms,draw_ms,active,batches,tris,culled,hiz_culled
#
# Process lifecycle mirrors meadow_sweep.sh exactly (mkfifo, background launch,
# kill trap on EXIT, FIFO quit, wait $PID) — but readiness is detected by
# polling the log for the FIFO-listener line (printed only after the world has
# fully loaded) instead of a fixed sleep, because the FIRST 500k bake can take
# minutes while cached runs load in seconds. Cap: 600 s per world.
#
# Usage: tools/stress_sweep.sh <stage-label>   e.g. tools/stress_sweep.sh stage4
set -euo pipefail
STAGE="${1:?usage: stress_sweep.sh <stage-label>}"
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE/../../MatterViewer"

CSV="$HERE/../docs/perf/stress_sweep.csv"
TODAY="$(date +%F)"
WORLDS="StressForest50k StressForest100k StressForest200k StressForest500k"

for WORLD in $WORLDS; do
  FIFO="/tmp/matter_stress_$$_${WORLD}.fifo"
  LOG="/tmp/matter_stress_$$_${WORLD}.log"
  mkfifo "$FIFO"
  # stdbuf -oL: stdout to a file is block-buffered, which would delay the
  # readiness marker ("MATTER_CMD_FIFO: listening") indefinitely; force
  # line-buffering so the log poll below sees it as soon as it prints.
  GALLIUM_DRIVER="${GALLIUM_DRIVER:-d3d12}" MATTER_GPU_CULL=1 \
    MATTER_WORLD="$WORLD" MATTER_CMD_FIFO="$FIFO" stdbuf -oL ./viewer > "$LOG" 2>&1 &
  PID=$!
  trap 'kill $PID 2>/dev/null || true; rm -f "$FIFO"' EXIT

  # Readiness: "MATTER_CMD_FIFO: listening" prints AFTER connect_sequence, so
  # the world is loaded once it appears. Bail early if the viewer died.
  READY=0
  for _ in $(seq 1 600); do
    if ! kill -0 "$PID" 2>/dev/null; then break; fi
    if grep -q 'MATTER_CMD_FIFO: listening' "$LOG"; then READY=1; break; fi
    sleep 1
  done
  if [ "$READY" != 1 ]; then
    echo "ERROR: $WORLD not ready after 600s (or viewer died). Log tail:"
    tail -n 10 "$LOG"
    kill "$PID" 2>/dev/null || true
    wait "$PID" 2>/dev/null || true
    rm -f "$FIFO"
    trap - EXIT
    exit 1
  fi
  sleep 3   # settle a few frames of steady state before the first camera

  run_cam() {  # name px py pz tx ty tz
    echo "cam $2 $3 $4 $5 $6 $7" > "$FIFO"
    sleep 2                        # let LOD/culling settle at the new view
    echo "stats $1" > "$FIFO"
    sleep 1
  }
  # Fixed camera set scaled from the meadow spec to the 2 km world (0..2000):
  run_cam aerial   1000 2000 -350  1000  0 1000    # whole forest in frustum
  run_cam corner     60   15   60   470  8  470    # in-crowd ground view
  run_cam midfield  310   45  310  1000 15 1000    # mid-distance
  run_cam far        30   25   30  1960  0 1960    # far ground view across the world
  run_cam empty    -300   40 -300 -1600 40 -1600   # outside, looking away: fixed CPU cost

  echo "quit" > "$FIFO"
  wait "$PID" || true
  trap - EXIT
  rm -f "$FIFO"

  grep '^STATS,' "$LOG" | while IFS= read -r line; do
    echo "$TODAY,$STAGE,$WORLD,${line#STATS,}" >> "$CSV"
  done
  # Keep the log (GpuCuller memory prints feed the P2 region-memory decision).
  echo "--- $WORLD done; log kept at $LOG"
done

echo "--- appended to $CSV:"
tail -n 20 "$CSV"
