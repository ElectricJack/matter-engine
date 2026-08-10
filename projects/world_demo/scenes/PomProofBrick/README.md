# PomProofBrick — reading ground POM off a sphere, with a ruler on it

`PomProof`'s three-dome instrument wearing `BrickProof` instead of `ForestFloor`:
a running-bond masonry with flat faces and deep, sharp-walled grooves, in place
of a stochastic pebble field. Same geometry, same cameras, same lights, same
single-material discipline. **The only variable is the detail texture.**

    MATTER_WORLD=PomProofBrick ./build/windows/editor.exe

Cold bake 19.7 s total (the `BrickProof` `.gtex` is 8.7 s of it); warm start
1.4 s. Add `MATTER_HIDE_UI=1` for full-frame 1280x720 captures, and
`MATTER_DISABLE_VK_RT=1` to make frames bit-deterministic for A/B work.

---

## Why swap the texture

`PomProof` isolated the variable but could not always read the answer, because
**noise hides the failures we are hunting**. A smeared pebble field still looks
like a pebble field. A brick course that bends, shears or tears does not.

| property | what the brick grid buys |
|---|---|
| straight edges | shear and smear become unmistakable, with no reference image needed |
| flat faces + deep grooves | the near lip must occlude the groove behind it; a strong, binary occlusion cue |
| a regular lattice | draws the surface parameterisation *geometrically* — see the warp section |
| authored depth | 0.12 m exactly, so displacement is checkable against a number |

## The tileset

`objects/BrickProof.js`. 2.0 m tile at 512 texels/m, matching `ForestFloor` and
the Alpine sets, so the two are directly comparable.

| quantity | value |
|---|---|
| courses per tile | 8 (course pitch 0.25 m) |
| bricks per course | 4 (brick pitch 0.50 m), running bond, half-brick offset |
| flat brick face | 0.40625 x 0.15625 m (2.6 : 1) |
| groove opening | 0.09375 m (3 base cells) |
| groove flat floor | 0.03125 m (1 base cell) |
| groove depth | 0.12 m |
| wall slope | 75.4 deg — the steepest the base grid can hold |
| per-brick jitter | 0 .. -0.02 m, downward only, hashed on (course, brick) |
| distinct bricks | 32 per tile; all 16 Wang tiles identical |

**The 64-sample constraint drives all of it.** `this.base()` is sampled on a
fixed `BaseField::kSamplesPerTile = 64` grid whatever `texelsPerMeter` says, so
the base cell is 2.0/64 = 3.125 cm. A wall cannot be steeper than one cell, and
a *flat-bottomed* groove needs two adjacent samples at floor depth — hence the
3-cell minimum groove. Joint centre lines are placed halfway between samples so
that two samples land at floor depth; on-sample joints would give a V, not a
channel. **No chamfer**: the smallest one this grid can express doubles the wall
run and halves the wall slope, softening exactly the occlusion edge under test.

Verified against the baked atlas (`MATTER_TILESET_DUMP_PNG=1`):

* wall runs measure 16-17 px = exactly one base cell — the bake adds no softening;
* the groove floor sits at **-0.1200 m**, the authored value, to four decimals;
* all 16 Wang tiles are **byte-identical** (max difference 0.0 code values);
* the wrap is exact. The height function is a composition of integer indices
  reduced modulo divisors of 64, so it is provably 2.0-periodic (audited over a
  [-8, 72)^2 sample sweep: 0 mismatches). Joints land *on* the tile boundary,
  so the seam is buried inside a groove. The 3.45 mm step measured across the
  join is not a seam: it is exactly half the 7.77 mm per-texel step on a ramp,
  the arithmetic of a texel straddling a mesh vertex.

### What `this.base()` allows — and per-brick colour

`base(fn, mat)` takes a height callback plus **one** material constant
(`dsl_bindings.cpp`, `j_ts_base`), and the atlas albedo is written as
`mat_albedo(mid)` from a per-triangle material id (`tileset_bake_primary.comp`)
— every triangle of the base carries that one id. **Per-brick albedo is not
expressible.** The only route to colour variation in a tileset is child geometry
(`layer` / `dropChild`), and both go through the physics settle, which tumbles
what it places — useless for laying a precise bond. `variant(fn)` runs per Wang
tile, not per brick, and varying it would break the toroidal continuity the grid
depends on.

That is a virtue here. With a uniform albedo every visible feature is relief, so
nothing can be mistaken for parallax. It also makes the near/far albedo path a
no-op: the near band modulates by `atlas_albedo / mean_albedo`, and for a
single-material base that ratio is exactly 1.0 everywhere.

---

## The height budget, measured

    height_min = -0.17   height_max = +0.05   h_range = 0.220 m

The bake pads (`hmax += 0.05`, `hmin -= 0.05`), and the shader anchors relief at
the datum `height_max`, so brick tops sit at raw -0.05 .. -0.07 and groove
floors at -0.17. Sweeping `render.pom.relief_cap_m` at a fixed camera:

| cap | % of frame differing from cap = 0.50 |
|---|---|
| 0.05 | 82.8 % (surface is perfectly FLAT — a free "POM off" control) |
| 0.10 | 80.4 % |
| 0.12 | 78.6 % |
| 0.15 | 72.3 % |
| 0.17 | 62.5 % |
| 0.18 | 50.3 % |
| **0.22** | **0.000 % — pixel-identical from here up** |
| 0.28 / 0.352 / 0.50 | 0.000 % |

Saturation is at `h_range`, **not** at groove depth + padding (0.17). The extra
travel is real and worth understanding: `relief` is used twice — it bounds the
clamp *and* is the numerator of `march_len = min(relief/cos_theta, max_march_m)`.
Above 0.17 the height field is frozen but the march keeps lengthening, changing
the step size and hence where the discrete march finds each groove edge. The
0.17 -> 0.18 step moves 53.6 % of the frame, and that change collapses to 0.2 %
above 84 deg — exactly where `max_march_m` becomes the binding term and `relief`
stops setting `march_len`.

---

## Does the visible smear start at `acos(relief / max_march_m)`?

This is the question `PomProof` left open, because its local-contrast metric
tracked texture magnification rather than smear. **Answer: the contour is real
and accurate, but it is not where visible smear begins.**

### The contour is confirmed to about a degree

Incidence is computed in closed form per pixel (eye (37,6,0), target (53,15,0),
vfov = pi/4, dome centre (85,1,0) R=40). That model independently reproduces
`PomProof.README.md`'s ladder — it puts 77.2 deg at image row 67, as published.

Diffing consecutive rungs of a `max_march_m` ladder isolates the pixels the cap
decided, with `relief` = 0.22 throughout:

| `max_march_m` | predicted onset | measured onset | error |
|---|---|---|---|
| 0.30 -> 0.40 | 42.8 deg | 35.0 deg | -7.8 |
| 0.40 -> 0.55 | 56.6 deg | 58.0 deg | +1.4 |
| 0.55 -> 0.75 | 66.4 deg | 67.0 deg | +0.6 |
| 0.75 -> 0.99 | 72.9 deg | 73.0 deg | +0.1 |
| 0.99 -> 1.30 | **77.2 deg** | **77.0 deg** | **-0.2** |
| 1.30 -> 1.59 | 80.3 deg | 80.0 deg | -0.3 |
| 1.59 -> 2.00 | 82.0 deg | 81.0 deg | -1.0 |

Per-degree, for the shipped default against the slider ceiling, the transition
is razor sharp at the predicted 82.0 deg:

    ... 79-80 deg 0.00%  |  80-81 deg 0.00%  |  81-82 deg 8.35%
        82-83 deg 65.81% |  83-84 deg 96.32% |  84-85 deg 99.05%

The 0.30 rung is the outlier at -7.8 deg. That is the expected slop between the
analytic sphere normal used here and the interpolated mesh gradient normal the
shader actually receives, which `PomProof.README.md` documents as capping at
78.8 deg on this dome.

### On flat ground the agreement is 0.2 degrees

The domes carry that mesh-normal slop; **the flat control ground does not**. It
is the plane `y = 1` with normal `(0,1,0)` exactly, so `cos_theta = |ray.y|`
with no interpolation anywhere. From an eye 2 m above it (`cam -60 3 0 -100 1
0`, no dome in frame) the frame is a clean incidence ruler from 64.7 deg in the
near foreground to 90 deg at the horizon:

| `max_march_m` | predicted onset | measured onset | error |
|---|---|---|---|
| 0.55 -> 0.99 | 66.4 deg | 66.5 deg | +0.1 |
| 0.99 -> 1.59 | **77.2 deg** | **77.0 deg** | **-0.2** |
| 1.59 -> 2.00 | 82.0 deg | 82.0 deg | **0.0** |

Per half-degree across the 77.2 deg contour, the step is essentially a
discontinuity:

    76.0-76.5 deg  0.00%   |   77.0-77.5 deg  19.72%
    76.5-77.0 deg  0.00%   |   77.5-78.0 deg  35.36%

(The 0.30 rung's 42.8 deg contour is off the bottom of this frame, whose
shallowest visible incidence is 64.7 deg, so that rung reads as "onset at the
frame edge" rather than as an error.)

**`acos(relief / max_march_m)` is exact.** Where it was previously confirmed
only to a couple of degrees, with exact normals it holds to 0.2 deg.

### But visible smear does not begin there

The un-marched remainder is **zero at the contour** and grows continuously. Its
lateral component — how far across the texture the returned texel sits from the
right one, which is what smear physically *is* — is
`(relief/cos(theta) - max_march_m) * sin(theta)`. Tabulated against the 0.25 m
course pitch and 0.50 m brick pitch:

| `max_march_m` | contour (row) | err = 0.25 m at | err = 0.50 m at |
|---|---|---|---|
| 0.30 | 42.8 deg (row 375) | 67.3 deg (row 134) | 74.4 deg (row 83) |
| 0.55 | 66.4 deg (row 141) | 74.2 deg (row 84) | 78.0 deg (row 63) |
| 0.99 | 77.2 deg (row 67) | 79.8 deg (row 55) | 81.5 deg (row 48) |
| 1.59 | 82.0 deg (row 46) | 83.1 deg (row 42) | 84.0 deg (row 40) |
| 2.00 | 83.7 deg (row 41) | 84.4 deg (row 39) | 85.0 deg (row 38) |

Moving `max_march_m` from 0.30 to 2.00 moves the **contour** by 334 image rows
but moves the row where the error reaches one brick by only 45. Because
`relief/cos(theta)` blows up so fast, texture-scale error is pinned near the
silhouette almost regardless of the knob. **At the shipped default the entire
cap-affected region is a 14-row sliver (rows 32-46) at the silhouette** — far
too small to read as a smear band.

Consistent with that, no metric or visual inspection localises a transition at
the contour. At `max_march_m` = 0.30, where the cap engages from mid-frame
(row 375), the lattice is still fully resolved there and for hundreds of rows
above it. A magnification-invariant lattice metric (max normalised
autocorrelation over lags 3-80 px, which tracks *periodicity* rather than
contrast) shows no collapse anywhere on the visible dome, at any rung.

The flat-ground ladder makes the point visually
(`M05_flatground_ladder_contour.png`, with each panel's contour drawn on it).
The grazing ground is pulled into long radial streaks converging on the
vanishing point — the smear everyone recognises — but **those streaks are just
as strong BELOW each panel's contour, where the cap is not engaged at all**, and
they are present in weaker form even with POM switched off entirely. That
identifies them as anisotropic footprint stretching (a filtering/LOD problem:
the texel footprint is enormously elongated along the view direction) rather
than as march truncation.

**So: `acos(relief/max_march_m)` predicts where the march changes, exactly. It
does not predict where the picture visibly degrades — those are two different
phenomena with different onsets, and the visible one starts much shallower.**
The two were conflated because on `ForestFloor` neither could be located
independently. Chasing the contour with `max_march_m` therefore does not fix
grazing smear; it was never the cause.

### An unexpected consequence: raising `max_march_m` makes it worse

At fixed `render.pom.steps` = 30, `max_march_m` sets the march step size:

| `max_march_m` | step | samples across a 0.094 m groove |
|---|---|---|
| 0.30 | 1.00 cm | 9.4 |
| 0.99 | 3.30 cm | 2.8 |
| 1.59 (default) | 5.30 cm | 1.8 |
| 2.00 (ceiling) | 6.67 cm | 1.4 |

At the default the march samples a groove fewer than twice, and at the ceiling
fewer than 1.5 times. In the grazing band the ladder is visibly *more* broken up
at 2.00 than at 0.30 (`M01_grazing_band_march_ladder.png`). Pushing
`max_march_m` up to chase the contour therefore buys a steeper contour at the
cost of undersampling the relief everywhere below it. This is legible only
because the groove has a known width; on noise there is nothing to undersample
*relative to*.

---

## What the warp does to the lattice

`MATTER_VT_WARP=0` reverts the march's addressing to the shipped world-XZ form.
It is read once per process, so warp on and warp off need two launches.

With a regular grid the difference is finally geometric rather than merely
present (`M04_warp_on_vs_off.png`, flank of the 40 m dome at 33-52 deg):

* **warp on** — courses run as clean, straight, continuous lines across the
  flank; brick size is uniform; the bond is unbroken.
* **warp off** — the lattice is **sheared and fragmented**. Courses no longer
  run straight: they break into short segments that step and offset against one
  another, brick size varies patch to patch, and there are visible
  discontinuities where adjacent patches fail to line up.

The warp does not distort the grid — **it repairs it.** Without it the world-XZ
addressing is discontinuous across the surface parameterisation, and the brick
lattice tears along those boundaries. That is the read `ForestFloor` could never
give: 58.8 % of the frame differs between the two, peaking at 93.0 % in the
70-80 deg band, but on a pebble field "different" and "correct" are
indistinguishable.

---

## Files

* `PomProofBrick.js` — the world, a deliberate sibling of `PomProof.js`. The
  field, camera, lights, streaming and biome blocks are meant to stay identical;
  see the header for the diff recipe that checks it.
* `../objects/BrickProof.js` — the tileset.
