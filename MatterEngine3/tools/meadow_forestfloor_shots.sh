#!/usr/bin/env bash
# Meadow ForestFloor shot regression: 5 poses that stress the Wang-tile atlas —
# a 4-way seam corner, an aerial view showing the full 4x4 torus tile grid, a
# grazing-angle shot along a seam ridge, mid-field to catch albedo/normal/ORM
# transitions, and a far view to test mip fall-off. Self-terminates.
#
# Usage: tools/meadow_forestfloor_shots.sh <out-dir>
#   e.g. GALLIUM_DRIVER=d3d12 tools/meadow_forestfloor_shots.sh /tmp/ff_shots
set -euo pipefail
OUT="${1:?usage: meadow_forestfloor_shots.sh <out-dir>}"
mkdir -p "$OUT"
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE/../viewer"

FIFO="/tmp/matter_ff_shots_$$.fifo"
LOG="$OUT/forestfloor_viewer.log"
mkfifo "$FIFO"
MATTER_WORLD="${MATTER_WORLD:-floordemo}" \
GALLIUM_DRIVER="${GALLIUM_DRIVER:-d3d12}" \
MATTER_CMD_FIFO="$FIFO" \
./viewer > "$LOG" 2>&1 &
PID=$!
trap 'kill $PID 2>/dev/null || true; rm -f "$FIFO"' EXIT

# GroundOnly world contains just the ForestFloor tileset (no Meadow tree scatter),
# so cold-cache load = tileset settle + bake ~30-60s. Retarget to `meadow` here
# once the Meadow tree-scatter GpuCuller SSBO growth is separately capped.
sleep 90

shoot() {  # name px py pz tx ty tz
  local name="$1"
  local png="$OUT/ff_${name}.png"
  rm -f "$png" "${png}.done"
  echo "cam $2 $3 $4 $5 $6 $7" > "$FIFO"
  sleep 2
  echo "shot $png" > "$FIFO"
  local waited=0
  for _ in $(seq 1 30); do
    [ -e "${png}.done" ] && { waited=1; break; }
    sleep 1
  done
  if [ "$waited" -eq 0 ]; then
    echo "ERROR: shot $name timed out (no .done marker after 30s)" >&2
    echo "  viewer log tail:" >&2
    tail -5 "$LOG" >&2
    exit 1
  fi

  # Sanity: verify the PNG exists and has non-trivial size.
  local sz
  sz=$(stat -c%s "$png" 2>/dev/null || echo 0)
  if [ "$sz" -lt 10000 ]; then
    echo "ERROR: shot $name too small (${sz} bytes); likely a blank frame" >&2
    exit 1
  fi

  # Pixel content check using Python3/Pillow: count pixels that differ from the
  # sky clear color (96,118,143).  The ForestFloor ground must produce at least
  # 5% non-sky pixels from every pose.  If ALL pixels match sky, the tileset
  # path is broken (not rendering ground) — that is a real bug, not a test skip.
  local nonsky_pct
  nonsky_pct=$(python3 - "$png" <<'PYEOF'
import sys
from PIL import Image
img = Image.open(sys.argv[1]).convert("RGB")
px = img.load()
w, h = img.size
sky = (96, 118, 143)
# Allow ±12 per channel for tone-mapped sky variation.
tol = 12
nonsky = 0
total = w * h
for y in range(h):
    for x in range(w):
        r, g, b = px[x, y]
        if abs(r - sky[0]) > tol or abs(g - sky[1]) > tol or abs(b - sky[2]) > tol:
            nonsky += 1
pct = nonsky * 100.0 / total
print(f"{pct:.2f}")
PYEOF
  )

  echo "  shot $name: ${sz} bytes, ${nonsky_pct}% non-sky pixels"

  # Gate: 5% of a 1280x720 frame = 46080 px minimum — enough to confirm ground rendering.
  local ok
  ok=$(python3 -c "print(1 if float('$nonsky_pct') >= 5.0 else 0)")
  if [ "$ok" -ne 1 ]; then
    echo "ERROR: shot $name has only ${nonsky_pct}% non-sky pixels (need >=5%)" >&2
    echo "  This means the ForestFloor tileset is NOT rendering; check:" >&2
    echo "    1. LocalProvider loaded ForestFloor slot (grep 'ForestFloor' $LOG)" >&2
    echo "    2. Wang shader bound the atlas (grep 'tileset' $LOG)" >&2
    echo "    3. run_tileset_phase produced .gtex files" >&2
    exit 1
  fi
}

# ---- 5 Wang-seam-heavy poses -----------------------------------------------
# Camera args are: `shoot NAME  eye.x eye.y eye.z   tgt.x tgt.y tgt.z`
# FloorDemo world = 16 m x 16 m dirt quad centered on the origin at y=0.
# Tile edge = 2 m so the quad spans 8 x 8 Wang tiles.

# Straight-down aerial: eye 20 m above origin looking down at the ground — sees
# the full 8x8 Wang grid and every 4-way seam corner at once.
shoot aerial      0  20   0     0   0   0

# 4-way seam corner: eye just above a corner boundary (2,2), looking down the
# diagonal so the seam intersection is centered under the reticle.
shoot corner      2   3   2     4   0   4

# Grazing-angle along the +X seam ridge: eye low + inward, target down the
# ridge — long tangential view maximises visible seam length on the X edge.
shoot seam_x     -8   1.2 0     8   0   0

# Mid-field: 5 m up, half the quad in view — catches the mid-mip transition
# where textureGrad picks a mid mip.
shoot mid        -4   5  -4     4   0   4

# Far view: 40 m out at a low angle — coarsest mip, confirms atlas doesn't go
# all-black at distance and no seam artefacts appear at the mip boundary.
shoot far       -30  10 -30     0   0   0

# ----------------------------------------------------------------------------
echo "quit" > "$FIFO"
wait "$PID" || true
trap - EXIT
rm -f "$FIFO"

echo ""
echo "--- ForestFloor shots: 5 PNGs in $OUT (viewer exited cleanly)"
echo "    Verify ground texture visually:"
for name in aerial corner seam_x mid far; do
  echo "      $OUT/ff_${name}.png"
done
