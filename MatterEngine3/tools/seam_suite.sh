#!/usr/bin/env bash
# seam_suite.sh -- the terrain SEAM test suite. Runs in the real editor, on the
# real streamed world, and reports every class of seam defect this repo has ever
# had a report for.
#
#     MatterEngine3/tools/seam_suite.sh <out-dir> [pose-file]
#
# Filed against issue da52492c. Every seam check that existed before this one
# was defined on synthetic geometry -- seam_weld_tests.cpp builds its own
# FaceRecords, seam_integration_tests.cpp meshes its own fixture pair -- and all
# of them passed on the frames users filed. This suite is defined on what the
# app actually draws, which is why it lives in tools/ and drives an editor
# rather than living in tests/ and calling a function.
#
# THE WORLD IS THE INSTRUMENT. It runs against SeamLab
# (projects/world_demo/scenes/SeamLab), whose density is a cave-free
# heightfield: every solid point below the surface is solid to yMin, so a camera
# above the terrain has nothing legitimate to see except the surface. That is
# what makes check 1 exact rather than heuristic. Pointing this suite at
# StreamCaverns would break check 1 (its surface is full of authored tunnel
# mouths) and nothing else.
#
# THE SIX CHECKS, and what each one is the only detector of:
#
#   1. holes      hole_scan.py -- background pixels enclosed by terrain. A
#                 see-through crack. Exact: gate is zero, no tolerance.
#   2. cracks     crack_scan.py -- thin depth SPIKES. Catches the crack that
#                 shows FARTHER TERRAIN rather than the sky, which check 1
#                 cannot see because the ray never reaches the background.
#   3. pool       MATTER_SEAM_TRACE -- the welder's own accounting invariants:
#                 level gaps, drawn level violations, build errors, sign
#                 conflicts. In-app, per frame, and zero-cost to collect.
#   4. shading    the frame with welds drawn against the same frame with
#                 MATTER_NO_SEAM_WELD_DRAW=1. Pixels the weld paints, and how
#                 much darker it paints them. This is the ONLY check that sees
#                 issue ec2829d6's class -- a seam that is geometrically closed
#                 and visibly wrong -- because its depth is continuous and it
#                 shows no background.
#   5. flicker    two DEPTH captures at one static pose, several seconds apart,
#                 must be bitwise identical. Catches z-fighting between the
#                 overlap band and the tile it overlaps, and any weld that
#                 rebuilds to different geometry from unchanged inputs. Depth
#                 rather than the lit frame because the lit frame carries the
#                 denoiser's temporal residual and the depth view carries none.
#   6. residue    the welder's own missing_landing / missing_coarse_pair /
#                 degenerate counts, which say how much of the seam the fan
#                 declined to close and therefore what the band is carrying
#                 alone.
#
# Output: <out-dir>/report.txt and <out-dir>/report.json, plus every capture.
# Exit status is 1 if any GATED check fails, 0 otherwise. The gates and the
# committed expectations live in seam_report.py.
set -u
OUT="${1:?usage: seam_suite.sh <out-dir> [pose-file]}"
HERE="$(cd "$(dirname "$0")" && pwd)"
POSES="${2:-$HERE/seam_lab_poses.txt}"
ED="$HERE/../../MatterEditor"
WORLD="${MATTER_WORLD:-SeamLab}"
# The settle pose. The anchor is frozen HERE, so it fixes where every ring
# boundary in the pose file is. Changing it invalidates the whole pose set.
ANCHOR="${SEAM_ANCHOR:-0,140,0,0,60,60}"
SETTLE="${SEAM_SETTLE:-20}"

mkdir -p "$OUT"
command -v python >/dev/null || { echo "seam_suite: python not on PATH" >&2; exit 2; }

# ---------------------------------------------------------------------------
# One editor run: launch, settle, freeze the anchor, walk the poses. `$1` is a
# label ("weld" / "noweld"); the caller exports whatever env it wants first.
# The lit shot is taken TWICE at each pose, several seconds apart, for check 5.
# ---------------------------------------------------------------------------
run_pass() {
    # One assignment per `local`: bash expands the whole statement's words
    # before it binds any of them, so `local a=$1 b=$OUT/$a` reads `a` unbound
    # and `set -u` kills the run.
    local label="$1"
    local noweld="$2"
    local dir="$OUT/$label"
    mkdir -p "$dir"
    local fifo="$dir/cmd.txt"
    local log="$dir/run.log"
    : > "$fifo"
    # Exported inside the subshell rather than written as an assignment prefix:
    # bash decides at PARSE time whether a word is an assignment, so a
    # `${noweld:+VAR=1}` prefix that expands to nothing makes the NEXT
    # assignment the command word ("MATTER_WORLD=SeamLab: command not found").
    (
        cd "$ED" || exit 1
        export MATTER_VSYNC=0 MATTER_HIDE_UI=1 MATTER_SEAM_TRACE=1
        export MATTER_WORLD="$WORLD" MATTER_CAM="$ANCHOR" MATTER_CMD_FIFO="$fifo"
        [ -n "$noweld" ] && export MATTER_NO_SEAM_WELD_DRAW=1
        exec ./build/windows/editor.exe > "$log" 2>&1
    ) &
    local pid=$!

    local n=0
    while [ $n -lt 400 ]; do
        grep -q "viewer: bake ready" "$log" 2>/dev/null && break
        kill -0 $pid 2>/dev/null || { echo "seam_suite: editor died in $label" >&2
                                      tail -20 "$log" >&2; return 1; }
        sleep 2; n=$((n + 2))
    done
    # Residency has to be COMPLETE before the anchor is frozen, or the pose set
    # is measuring a half-streamed world and every run measures a different one.
    sleep "$SETTLE"
    echo "set viewer.debug.freeze_stream_anchor true" >> "$fifo"
    sleep 2

    local name a b c d e f
    while read -r name a b c d e f; do
        [ -z "${name:-}" ] && continue
        case "$name" in \#*) continue ;; esac
        echo "cam $a $b $c $d $e $f" >> "$fifo"
        sleep 4
        echo "stats $name" >> "$fifo"
        shoot "$fifo" "$dir/${name}_lit.png" 0
        shoot "$fifo" "$dir/${name}_depth.png" 2
        # Check 5's second sample: the SAME DEPTH view, ~5 s later, nothing
        # touched. Depth, not the lit frame, because the lit frame goes through
        # the RT denoiser and a temporal resolve -- on a static camera those
        # leave a low-amplitude residual everywhere, which buries the signal
        # (measured: 136 px differing by up to 24/255 on a frame with no
        # z-fighting at all). The depth view is written straight from the depth
        # buffer with no temporal filter, so two captures of an unchanged scene
        # must be BITWISE equal, and any pixel that is not is a surface that
        # won the depth test one frame and lost it the next.
        sleep 5
        shoot "$fifo" "$dir/${name}_depth2.png" 2
        echo "  $label/$name"
    done < "$POSES"

    echo "quit" >> "$fifo"
    sleep 4
    kill $pid 2>/dev/null
    grep '^STATS,' "$log" > "$dir/stats.log" 2>/dev/null || true
    grep '^\[seam\]' "$log" > "$dir/seam.log" 2>/dev/null || true
    return 0
}

# shoot <fifo> <png> <debug_view_mode>
shoot() {
    local fifo="$1" png="$2" mode="$3"
    rm -f "$png" "$png.done"
    echo "set viewer.debug.debug_view_mode $mode" >> "$fifo"
    sleep 2
    echo "shot $png" >> "$fifo"
    local i=0
    while [ $i -lt 30 ]; do [ -e "$png.done" ] && break; sleep 1; i=$((i + 1)); done
    [ -e "$png.done" ] || echo "seam_suite: shot timed out: $png" >&2
}

echo "== seam_suite: pass 1/2 (welds drawn) =="
run_pass weld "" || exit 2
echo "== seam_suite: pass 2/2 (MATTER_NO_SEAM_WELD_DRAW=1) =="
run_pass noweld 1 || exit 2

echo "== seam_suite: analysis =="
python "$HERE/seam_report.py" "$OUT" --poses "$POSES" \
    --baseline "$HERE/seam_baseline.json" | tee "$OUT/report.txt"
exit "${PIPESTATUS[0]}"
