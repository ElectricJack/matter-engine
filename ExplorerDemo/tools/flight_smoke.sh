#!/usr/bin/env bash
# flight_smoke.sh — endless-flight smoke gate for ExplorerDemo (Task 11).
#
# Launches the explorer in fly mode for 180 seconds, moving the camera
# along +X at 40 units/sec at cruise altitude (sea_level + 25). Verifies:
#   1. Zero "bake error" lines (case-insensitive)
#   2. fps_summary present in log
#   3. resident_sectors reported (value > 0)
#   4. "smoke done" line present (run reached full duration, no crash)
#
# Usage (from repo root or ExplorerDemo/):
#   GALLIUM_DRIVER=d3d12 bash ExplorerDemo/tools/flight_smoke.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXPLORER_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

if [ "${GALLIUM_DRIVER:-}" != "d3d12" ]; then
    echo "WARNING: GALLIUM_DRIVER is not d3d12; GPU may fall back to llvmpipe."
fi

EXPLORER_BIN="$EXPLORER_DIR/explorer"
if [ ! -x "$EXPLORER_BIN" ]; then
    echo "ERROR: $EXPLORER_BIN missing. Build first: make -C ExplorerDemo"
    exit 1
fi

LOG_DIR="$EXPLORER_DIR/logs"
mkdir -p "$LOG_DIR"
LOGFILE="$LOG_DIR/flight_smoke.log"

echo "============================================================"
echo "  ExplorerDemo flight_smoke.sh"
echo "  GALLIUM_DRIVER=${GALLIUM_DRIVER:-NOT_SET}"
echo "  Log: $LOGFILE"
echo "  Date: $(date)"
echo "============================================================"
echo

echo "--- Launching explorer with fly=1,0,40 for 180s ---"

(
    cd "$EXPLORER_DIR"
    GALLIUM_DRIVER="${GALLIUM_DRIVER:-d3d12}" \
    EXPLORER_SMOKE="secs=180,fly=1,0,40" \
    ./explorer 2>&1
) | tee "$LOGFILE"

echo
echo "--- Validating log ---"

FAIL=0

# 1. Zero bake error lines
BAKE_ERRORS=$(grep -ci "bake error" "$LOGFILE" || true)
if [ "$BAKE_ERRORS" -gt 0 ]; then
    echo "FAIL: $BAKE_ERRORS bake error line(s) found"
    FAIL=1
else
    echo "PASS: zero bake errors"
fi

# 2. fps_summary present
if grep -q "fps_summary" "$LOGFILE"; then
    echo "PASS: fps_summary present"
    FPS_LINE=$(grep "fps_summary" "$LOGFILE")
    echo "  $FPS_LINE"
else
    echo "FAIL: fps_summary not found in log"
    FAIL=1
fi

# 3. resident_sectors present and > 0
RS=$(grep -oP 'resident_sectors=\K[0-9]+' "$LOGFILE" || true)
if [ -n "$RS" ] && [ "$RS" -gt 0 ]; then
    echo "PASS: resident_sectors=$RS"
else
    echo "FAIL: resident_sectors missing or zero (got: '${RS:-MISSING}')"
    FAIL=1
fi

# 4. smoke done line present (run completed without crash)
if grep -q "smoke done" "$LOGFILE"; then
    echo "PASS: smoke done (run completed)"
else
    echo "FAIL: 'smoke done' not found — run may have crashed"
    FAIL=1
fi

echo
if [ "$FAIL" -eq 0 ]; then
    echo "============================================================"
    echo "  FLIGHT SMOKE: ALL PASS"
    echo "============================================================"
    exit 0
else
    echo "============================================================"
    echo "  FLIGHT SMOKE: FAILED"
    echo "============================================================"
    exit 1
fi
