# The terrain seam suite, and what it found

Filed against issue `da52492c` — "with terrain streaming disabled, it's easy to
see that sector borders are not correctly connecting", StreamCaverns, streaming
frozen, 2026-08-13.

## Why a new world

StreamCaverns cannot answer that question. Its density is a 3D field with a
tunnel network that breaks through the surface wherever it happens to reach it,
so the plateau is genuinely full of holes and a dark patch in a screenshot is
exactly as likely to be a tunnel mouth as a crack. Every seam measurement taken
on it has to argue about which — and the previous two rounds of this
investigation (issues `ec2829d6`, `81dd722b`) each spent most of their length on
that argument.

`projects/world_demo/scenes/SeamLab` removes the ambiguity by removing the
caves:

* **Cave-free heightfield.** Density is `h(x, z) - y`. Every solid point below
  the surface is solid to `yMin`, so a camera above the terrain has nothing
  legitimate to see except the surface and the sky above the horizon. A
  background pixel enclosed by terrain is a hole, with **no tolerance and no
  threshold**. That is the check StreamCaverns can never have.
* **Cube tiles anyway.** `volumetricSectors: true`, so seams exist on all six
  faces and the Y-tiled mesher path is under test — the same regime the issue
  was filed against. Heightfield density plus cube tiles is the cheap corner of
  that regime, not a different one.
* **Small.** 4 levels, 496 m reach, `sectorSize` 32 against the usual 64, so
  every tile at every level is 16³ cells — an eighth of what the 64 m worlds
  bake. The world fills in seconds, which is what makes it a fixture rather than
  a soak.
* **Relief at every voxel scale.** One octave set per rung in the ladder (420 m,
  110 m, 26 m, 7 m) plus a 24 m scarp, because a seam opens in proportion to how
  much of the height lives below the coarse voxel. A smooth world hides the
  defect.
* **A 32 m checker in the tape.** 32 m is the level-0 tile size and every
  coarser tile is a multiple of it, so a checker cell edge lies on a tile border
  at every level. A screenshot shows the tile grid without a debug view.

No scatter, no tilesets, no fog, no volumetrics: each of those would either
stand in front of the thing being looked at or tint the background the oracle
thresholds on.

## The suite

    MatterEngine3/tools/seam_suite.sh <out-dir> [pose-file]

Two editor runs over `seam_lab_poses.txt` (7 poses, anchor frozen at the settle
pose so the LOD ring boundaries are fixed spheres), one with welds drawn and one
under `MATTER_NO_SEAM_WELD_DRAW=1`. `seam_report.py` produces the table and the
gates.

| check | instrument | what only it sees | gate |
|---|---|---|---|
| `holes` | `hole_scan.py` | background pixels enclosed by terrain: a see-through crack | 0 |
| `cracks` | `crack_scan.py` | thin depth spikes: a crack that shows farther TERRAIN, which never reaches the background | 0 |
| `flicker` | two depth captures, one static pose | z-fighting between the band and the tile it overlaps; a weld that rebuilds differently from unchanged inputs | 0 |
| `shading` | weld frame vs `MATTER_NO_SEAM_WELD_DRAW=1` frame | a seam that is geometrically closed and visibly wrong (issue `ec2829d6`'s class) — depth is continuous, so nothing above sees it | ratio ≥ 0.80 |
| `pool` | `MATTER_SEAM_TRACE` | the welder's own accounting invariants: level gaps, drawn level violations, build errors, sign conflicts | 0 |
| `residue` | the welder's `missing_landing` / `degenerate` | how much of the seam the fan declined to close, and therefore what the band is carrying alone | reported |

Two decisions in there are worth keeping:

**Flicker is measured on the depth view, not the lit frame.** The lit frame goes
through the RT denoiser and a temporal resolve, which on a static camera leave a
low-amplitude residual everywhere — 136 px differing by up to 24/255 on a frame
with no z-fighting at all, which buries the signal. The depth view is written
straight from the depth buffer with no temporal filter, so two captures of an
unchanged scene must be *bitwise* equal.

**`shading` gates a ratio, not a pixel count.** The weld is supposed to change
pixels; that is what drawing the band is. What is a defect is the weld painting
terrain darker than the terrain around it, which is what a wrong material or a
reversed normal looks like. The reference is the ring of pixels just outside the
changed set in the same frame — not the no-weld frame, where those pixels show
whatever the band exists to cover.

`seam_locate.py` closes the loop from a pixel to a fix: it inverts
`composite.frag`'s `compute_view_ray` (including its world-up flip at
|fwd.y| > 0.999, which the straight-down poses are exactly the case for),
reconstructs the world position of each defect, and reports its distance to the
nearest tile border of every level. That is what turned "252 hole pixels" into
"every one of them within 0.3 m of the y = 64 plane".

## Finding 1 — every horizontal cross-level seam was unwelded

`WorldSession::Impl::rebuild_weld_pair` opened with

    if (axis == 1) { drop(); return; }          // +-y untiled in M0

true when it was written, and false from the moment M3 made every tile a cube.
Everything a horizontal cross-level seam needs had already been built for it:
`kSeamFaces` enumerates ±y under `seam_face_count()`, `face_neighbour_span`
spans a y-normal face, the mesher exports the ±y `FaceRecord`s and the -y
overlap band, `side_at` resolves a y-normal face's two horizontal tangents, and
the global y cell index is anchored at world y = 0 so `coarse = floor_div2(fine)`
holds on the vertical axis exactly as it does on x and z. This one line dropped
every such pair before any of it ran.

Where it showed: a fine tile sitting on top of a coarse one bridges *downward*
and under-reaches by half a coarse voxel, which is what the -y band exists to
cover. Unwelded, that is an open strip one coarse voxel wide along the whole
plane — and because the surface is nearly tangent to a horizontal plane wherever
it crosses one, the strip is wide in plan view. In SeamLab the holes were two
gashes ~6 m × 19 m at (-31, 64, 60), and **every hole component at every pose
was the same physical defect seen from a different angle**.

Fixed by deleting the line. The column path stays excluded by
`face_neighbour_span`'s own guard, which is the mechanism that actually knows
whether the tile above a tile is a different tile.

| pose | `down` | `down_lo` | `r1` | `r2` | `r3` | `graze` | `obliq` |
|---|---|---|---|---|---|---|---|
| hole px before | 252 | 582 | 0 | 268 | 56 | 0 | 196 |
| hole px after | 0 | 0 | 0 | 0 | 0 | 0 | 0 |

The welder's own counters said nothing was wrong the whole time — `gap=0 err=0
viol=0 sign=0` on every frame — because a pair that is dropped before it is
built is a pair that reports nothing. That is the general lesson the previous
two rounds also produced, and the reason this suite is defined on the output.

## Finding 2 — the weld was shadowing itself

With the holes closed, what was left were thin dark-blue slivers along the
remaining seams — the same thing issue `ec2829d6` was filed as, and the residual
that investigation recorded as *unexplained*: "the strip still sits at 0.63-0.88
of its neighbours' luminance … either a genuinely near-vertical step ribbon or
wrong normals reaching the GPU. Pixels could not separate these."

They can, with the right four views. Measured on one strip of 336 pixels, weld
frame against `MATTER_NO_SEAM_WELD_DRAW=1`:

| | on the strip | just beside it |
|---|---|---|
| depth (view 2) | 33.06 m | 33.06 m — **and identical with the weld off** |
| raw albedo (view 4) | 189, 183, 173 | 189, 183, 173 |
| normals (view 1) | 81, 158, 127 | 96, 161, 126 |
| **sun visibility (view 3)** | **0.1 / 255** | **164.5 / 255** |

Same depth, same material, same normal, no light. Not a step, not a reversal,
not a classification miss: the strip is **fully shadowed**, and the only thing
that changes when the weld is not drawn is that those pixels come back lit at
178 instead of 30.

The occluder is the weld itself. A weld is coincident with the two tiles it
joins by construction — the fan interpolates between their own boundary vertices
and the band *is* the fine tile's surface laid over the coarse tile's, within
centimetres of it — so a shadow ray cast from it hits its own twin at t ≈ 0.
Every weld pixel is therefore in its own shadow, uniformly, which is why the
result is a clean black line rather than the speckle bias artefacts usually
produce.

Fixed by keeping welds out of the ray-tracing TLAS, which is what the CPU tracer
already does for an unrelated reason (`ensure_tracer`). A weld can occlude
nothing the tiles do not already occlude, so it loses nothing. `VkSceneInstance`
gained `ray_traced`, `matter_engine.cpp` clears it on the weld branch, and
`update_instances` skips the `rt_instances_` entry.

Dark weld pixels, gated per pose: **336 → 0** at the close pose, and 0 at every
suite pose but one (`graze`, 1 px).

The suite's `shading` check did not catch this on its own the first time, and
that is recorded in `seam_report.py`: it compared the *median* luminance of the
~10,000 pixels a weld touches, which those 336 black ones moved by 0.01. A
hairline is a small fraction of a large set by construction. The check now also
counts pixels individually darker than 0.55× **the same pixel in the no-weld
frame**, restricted to pixels where the two runs agree about depth — which is
what makes it a statement about shading rather than about geometry.

## Where it stands

    pose         holes  cracks  flicker   weldpx  ratio   dark
    down             0       0        0    29916   0.99      0
    down_lo          0       0        0    16937   0.99      0
    r1               0      10        0     8820   1.08      0
    r2               0      12        0    18627   0.99      0
    r3               0       0        0    10378   1.00      0
    graze            0       9        0    11458   0.98      1
    obliq            0      29        0    12824   1.15      0

    pool: gap=0 err=0 viol=0 sign=0 degen=0 | pairs=25 drawn=25 miss=15

Holes, flicker and dark weld pixels are all at zero and gated there. `cracks` is
gated against `seam_baseline.json` instead, and the reason is in that file: it is
the instrument's resolution, not a tolerance for a known defect. Every surviving
component was located, and most sit 1.5–11 m from the nearest tile border of any
level — ordinary terrain crease read as a spike by a detector that has to infer
"went through the shell" from a depth profile. `hole_scan`, which does not have
to infer anything, reads 0.

Not chased, and worth knowing about:

* `missing_landing` sits at 15 of 1042 crossings. That is the fan declining to
  invent a landing where the coarse side has no vertex, and the overlap band
  covering it instead — design §4.1's honest residue, working as specified.
* On StreamCaverns the pool is 171 pairs against 790 drawn sectors with
  `gap=0 err=0 viol=0`, well inside the 4×-drawn-tiles bound, so admitting the
  ±y faces did not make the pool grow the way a keying bug would.

## Running it

    MatterEngine3/tools/seam_suite.sh /tmp/seam

Takes about eight minutes (two editor runs, seven poses, two of them settling
the whole world). Everything it captures stays in the out-dir, so a failing
check can be looked at rather than re-run.
