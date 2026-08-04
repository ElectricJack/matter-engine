#!/usr/bin/env bash
# capture-replay-baseline.sh — capture a MATTER_REPLAY shot for milestone gating.
#
# WHY COLD CACHE: projects/<proj>/.cache is content-addressed on the world's JS
# SOURCE, not on engine code. A replay taken after an engine change with a warm
# cache reloads the previously baked geometry and reproduces the old pixels
# exactly — it passes while proving nothing. Every capture here wipes the cache
# so the comparison exercises the whole bake+render path.
#
# LAUNCH RULES (both learned the hard way):
#   * Run the exe with TMP/TEMP as the command's OWN env prefix, from a shell
#     that has NOT exported the MSYS2 UCRT64 PATH. A native Windows exe does not
#     inherit them otherwise, fs::temp_directory_path() falls back to
#     C:\WINDOWS, and fixtures fail with confusing "coherent load failed" errors.
#   * The editor must be launched from the MatterEditor/ working directory.
#   * Never build while an editor.exe is running: it holds a file lock.
#
# Usage:  bash docs/baselines/capture-replay-baseline.sh <issue-dir> <out.png> [shot-index]
#         (shot index is 1-BASED; default 1)

set -euo pipefail

ISSUE_DIR="${1:?usage: capture-replay-baseline.sh <issue-dir> <out.png> [shot-index]}"
OUT_PNG="${2:?missing output png}"
SHOT_INDEX="${3:-1}"

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
STATE_JSON="$ISSUE_DIR/state.json"
[ -f "$STATE_JSON" ] || { echo "no state.json at $STATE_JSON" >&2; exit 1; }

WORLD="$(python -c "import json,sys;print(json.load(open(sys.argv[1]))['world'])" "$STATE_JSON")"
echo "world=$WORLD shot=$SHOT_INDEX -> $OUT_PNG"

# Cold cache for this world only; leaves other worlds' bakes intact.
CACHE_DIR="$REPO_ROOT/projects/world_demo/.cache/$WORLD"
if [ -d "$CACHE_DIR" ]; then
    echo "wiping $CACHE_DIR"
    rm -rf "$CACHE_DIR"
fi

mkdir -p "$(dirname "$OUT_PNG")"
OUT_ABS="$(cd "$(dirname "$OUT_PNG")" && pwd)/$(basename "$OUT_PNG")"
STATE_ABS="$(cd "$(dirname "$STATE_JSON")" && pwd)/state.json"

cd "$REPO_ROOT/MatterEditor"
TMP="C:/Users/webde/AppData/Local/Temp" \
TEMP="C:/Users/webde/AppData/Local/Temp" \
MATTER_REPLAY="$STATE_ABS" \
MATTER_REPLAY_SHOT="$SHOT_INDEX" \
MATTER_REPLAY_OUT="$OUT_ABS" \
    ./build/windows/editor.exe

[ -f "$OUT_ABS" ] || { echo "FAILED: no output at $OUT_ABS" >&2; exit 1; }
echo "captured $OUT_ABS"
