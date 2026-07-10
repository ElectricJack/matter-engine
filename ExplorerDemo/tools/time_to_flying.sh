#!/usr/bin/env bash
# time_to_flying.sh — wall-clock gate for ExplorerDemo (Task 12).
#
# Measures per run:
#   t_ready      = elapsed seconds to "explorer: ready" (first rendered frame
#                  after BakeStarted — visible world appears)
#   t_silhouette = elapsed seconds to "bake finished" (BakeFinished event;
#                  the gate metric per the Task 12 spec)
# plus the fps_summary line (min/avg) printed at smoke exit.
#
# Runs:
#   1 cold run  (cache cleared first; secs=300)
#   1 warm run  (cache intact from the cold run; secs=90)
#
# Usage (from repo root or ExplorerDemo/):
#   GALLIUM_DRIVER=d3d12 bash ExplorerDemo/tools/time_to_flying.sh
#
# Gate (informational — final adjudication by Jack):
#   Cold t_silhouette: ~163-200s expected (JS install bottleneck)
#   Warm t_silhouette: target <=60s (shipped relaunch UX)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXPLORER_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

if [ "${GALLIUM_DRIVER:-}" != "d3d12" ]; then
    echo "WARNING: GALLIUM_DRIVER is not d3d12; GPU may fall back to llvmpipe."
fi

CACHE_DIR="$EXPLORER_DIR/cache"
LOG_DIR="$EXPLORER_DIR/logs"
mkdir -p "$LOG_DIR"

EXPLORER_BIN="$EXPLORER_DIR/explorer"
if [ ! -x "$EXPLORER_BIN" ]; then
    echo "ERROR: $EXPLORER_BIN missing. Build first: make -C ExplorerDemo"
    exit 1
fi

# run_smoke <label> <secs> <shot-path-or-empty>
# Prepends elapsed seconds (1s resolution via gawk systime) to each log line.
# Results via globals RUN_T_READY / RUN_T_SILHOUETTE / RUN_FPS.
run_smoke() {
    local label="$1" secs="$2" shot="${3:-}"
    local logfile="$LOG_DIR/${label}.log"

    echo
    echo "=== Run: $label (secs=$secs) ==="

    local smoke_env="secs=$secs"
    [ -n "$shot" ] && smoke_env="${smoke_env},shot=${shot}"

    (
        cd "$EXPLORER_DIR"
        GALLIUM_DRIVER="${GALLIUM_DRIVER:-d3d12}" \
        EXPLORER_SMOKE="$smoke_env" \
        ./explorer 2>&1
    ) | awk 'BEGIN { t0 = systime() } { printf "%d %s\n", systime() - t0, $0; fflush() }' \
      > "$logfile"

    RUN_T_READY=$(grep -m1 "explorer: ready" "$logfile" | awk '{print $1}' || true)
    RUN_T_SILHOUETTE=$(grep -m1 "bake finished" "$logfile" | awk '{print $1}' || true)
    RUN_FPS=$(grep -m1 "explorer: fps_summary" "$logfile" | cut -d' ' -f2- || true)
    RUN_T_READY="${RUN_T_READY:-MISSING}"
    RUN_T_SILHOUETTE="${RUN_T_SILHOUETTE:-MISSING}"
    RUN_FPS="${RUN_FPS:-MISSING}"

    echo "  --> t_ready=${RUN_T_READY}s  t_silhouette=${RUN_T_SILHOUETTE}s"
    echo "  --> ${RUN_FPS}"
}

echo "============================================================"
echo "  ExplorerDemo time_to_flying.sh"
echo "  GALLIUM_DRIVER=${GALLIUM_DRIVER:-NOT_SET}"
echo "  Log dir: $LOG_DIR"
echo "  Date:    $(date)"
echo "============================================================"

echo
echo "--- Cold run (cache cleared) ---"
rm -rf "$CACHE_DIR"
run_smoke "cold-1" 300 "/tmp/explorer_cold_run1.png"
COLD_READY="$RUN_T_READY"; COLD_SIL="$RUN_T_SILHOUETTE"; COLD_FPS="$RUN_FPS"

echo
echo "--- Warm run (cache intact) ---"
run_smoke "warm-1" 90 "/tmp/explorer_warm_run1.png"
WARM_READY="$RUN_T_READY"; WARM_SIL="$RUN_T_SILHOUETTE"; WARM_FPS="$RUN_FPS"

echo
echo "============================================================"
echo "  RESULTS SUMMARY"
echo "============================================================"
echo "  Cold: t_ready=${COLD_READY}s  t_silhouette=${COLD_SIL}s"
echo "        ${COLD_FPS}"
echo "        Gate: plan says <=60s cold; JS-install bottleneck makes"
echo "        ~163-200s expected — adjudication by Jack"
echo "  Warm: t_ready=${WARM_READY}s  t_silhouette=${WARM_SIL}s"
echo "        ${WARM_FPS}"
echo "        Gate: <=60s WARM (shipped relaunch UX)"
echo
echo "  Logs: $LOG_DIR/   Shots: /tmp/explorer_cold_run1.png /tmp/explorer_warm_run1.png"
echo "============================================================"
