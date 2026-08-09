#!/usr/bin/env bash
# One-editor visual/performance capture harness for atmosphere/cloud milestones.
# Usage: tools/atmosphere_cloud_shots.sh <suite> <label> <out-dir>
set -euo pipefail
SUITE="${1:?usage: atmosphere_cloud_shots.sh <suite> <label> <out-dir>}"
LABEL="${2:?usage: atmosphere_cloud_shots.sh <suite> <label> <out-dir>}"
OUT="${3:?usage: atmosphere_cloud_shots.sh <suite> <label> <out-dir>}"
mkdir -p "$OUT"
FIFO="/tmp/matter_atmosphere_clouds_$$.fifo"
LOG="$OUT/${LABEL}_viewer.log"
COMMANDS="$OUT/${LABEL}_commands.log"

send() { printf '%s\n' "$*" | tee -a "$COMMANDS" > "$FIFO"; }
capture() {
  local name="$1" png="$OUT/${LABEL}_${1}.png"
  rm -f "$png" "${png}.done"
  send "stats $name"
  send "shot $png"
  for _ in $(seq 1 60); do [ -e "${png}.done" ] && return; sleep 1; done
  echo "ERROR: screenshot timed out: $png" >&2
  exit 1
}

HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE/../../MatterEditor"
mkfifo "$FIFO"
: > "$COMMANDS"
PID=""
cleanup() {
  if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
    send "quit" || true
    wait "$PID" || true
  fi
  rm -f "$FIFO"
}
trap cleanup EXIT INT TERM

MATTER_WORLD="${MATTER_WORLD:-meadow}" \
MATTER_CMD_FIFO="$FIFO" \
TMP="${TMP:?TMP must be set for the Windows editor}" \
TEMP="${TEMP:?TEMP must be set for the Windows editor}" \
stdbuf -oL ./build/windows/editor.exe > "$LOG" 2>&1 &
PID=$!

# Both markers matter: baking can finish before the command transport listens.
READY=0
for _ in $(seq 1 300); do
  if ! kill -0 "$PID" 2>/dev/null; then break; fi
  if grep -q 'viewer: bake ready' "$LOG" 2>/dev/null && \
     grep -q 'MATTER_CMD_FIFO: listening' "$LOG" 2>/dev/null; then
    READY=1
    break
  fi
  sleep 1
done
if [ "$READY" != 1 ]; then
  echo "ERROR: viewer did not report bake and FIFO readiness. Log tail:" >&2
  tail -n 20 "$LOG" >&2 || true
  exit 1
fi

# Keep suite names stable; subsequent milestones add their property batches here.
case "$SUITE" in
  baseline)
    send "get render.volumetrics.enabled"
    send "cam 128 260 -40 128 0 128"
    sleep 2
    capture procedural_sky
    ;;
  atmosphere|froxel|cloud-lighting|cloud-shadows|final)
    echo "ERROR: suite '$SUITE' is reserved for a later milestone" >&2
    exit 2
    ;;
  *)
    echo "ERROR: unknown suite '$SUITE' (baseline, atmosphere, froxel, cloud-lighting, cloud-shadows, final)" >&2
    exit 2
    ;;
esac

send "quit"
wait "$PID"
PID=""
rm -f "$FIFO"
trap - EXIT INT TERM

grep '^STATS,' "$LOG" > "$OUT/${LABEL}_stats.log" || true
grep '"gpu_volumetrics_ms"' "$LOG" > "$OUT/${LABEL}_metrics.log" || true
echo "--- $LABEL: $SUITE capture and telemetry in $OUT"
