#!/usr/bin/env bash
# seam-soak.sh — the volumetric-sectors M0 acceptance run.
#
# docs/volumetric-sectors-design-2026-08-10.md §6 (M0) asks for "a StreamCaverns
# + StreamMountain LOD-churn soak (scripted fly-through toggling bands) with a
# missing-strip counter derived from seam_stats". This is that run. It is not a
# new harness: MATTER_CAM_PATH (MatterEditor/src/main.cpp) already drives a
# scripted fly-through one pose per RENDERED frame, and MATTER_SEAM_TRACE is
# ~100 lines in the same file that poll WorldSession::seam_weld_status() beside
# session->frame_stats() and print a verdict at the end of the path.
#
# WHY THE PATHS ARE HORIZONTAL. M0 is still the XZ-columnar grid: the streaming
# anchor is 2-D (Coordinator::submit_anchor takes (x, z); camera Y is discarded)
# and every band radius is a horizontal distance. A descent into a StreamCaverns
# shaft moves the anchor NOT AT ALL, so nothing splits, nothing merges, no
# cross-level face pair is created, and the welder sits at zero while the run
# "passes" having tested nothing. Y becomes a streaming axis in M3.
# streamcaverns_flythrough.path ends with a labelled 200-pose descent purely to
# demonstrate that.
#
# LAUNCH RULES (from capture-replay-baseline.sh, both learned the hard way):
#   * Run the exe with TMP/TEMP as the command's OWN env prefix, from a shell
#     that has NOT exported the MSYS2 UCRT64 PATH. A native Windows exe does not
#     inherit them otherwise and fs::temp_directory_path() falls back to
#     C:\WINDOWS, where the failure surfaces much later as something unrelated.
#   * The editor must be launched from the MatterEditor/ working directory.
#   * Never build while an editor.exe is running: it holds a file lock.
#
# CACHE. Unlike capture-replay-baseline.sh this does NOT wipe the cache. A soak
# measures the welder across LOD churn, and a cold cache just means the streamer
# runs a long way behind the anchor — which inflates the transients this run is
# trying to characterise (level_holds, missing_landing) without telling you
# anything about the welder. Run it twice if you want both readings.
#
# Usage:  bash docs/baselines/seam-soak.sh caverns|mountain [logfile]

set -euo pipefail

WHICH="${1:?usage: seam-soak.sh caverns|mountain [logfile]}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

case "$WHICH" in
  caverns)
    WORLD=StreamCaverns
    # Authored for this run: 2400 m +X, a still yaw (the control), 2400 m +Z,
    # a fast diagonal, then the labelled vertical descent.
    PATH_FILE="$REPO_ROOT/MatterEngine3/tools/streamcaverns_flythrough.path"
    ;;
  mountain)
    WORLD=StreamMountain
    # Already committed (3000 poses). NOTE its travel is only ~375 m total at
    # 0.125 m/pose — it was authored as a profiler-breakdown capture, not as a
    # churn path, so it exercises the welder far more gently than the caverns
    # one does. StreamMountain also rebakes ~6547 sectors from cold; let it run.
    PATH_FILE="$REPO_ROOT/MatterEngine3/tools/streammountain_flythrough.path"
    ;;
  *) echo "unknown soak '$WHICH' (caverns|mountain)" >&2; exit 1 ;;
esac

LOG="${2:-$REPO_ROOT/seam-soak-$WHICH.log}"
echo "world=$WORLD path=$PATH_FILE -> $LOG"

cd "$REPO_ROOT/MatterEditor"
TMP="C:/Users/webde/AppData/Local/Temp" \
TEMP="C:/Users/webde/AppData/Local/Temp" \
MATTER_WORLD="$WORLD" \
MATTER_SEAM_TRACE=1 \
MATTER_CAM_PATH="$PATH_FILE" \
MATTER_CAM_PATH_EXIT=1 \
MATTER_CAM_PATH_WARMUP=300 \
    ./build/windows/editor.exe > "$LOG" 2>&1 || true

# The editor's own exit code covers Vulkan validation and fatal errors, not the
# soak verdict — the seam summary is the verdict, and it is a block of text.
# Grepping it here is what gives the run an exit code.
sed -n '/==== seam-weld summary/,/==== end seam-weld summary/p' "$LOG"
if grep -q "VERDICT: PASS" "$LOG"; then
    echo "seam soak: PASS"
else
    echo "seam soak: FAIL (or no verdict — check $LOG)" >&2
    exit 1
fi
