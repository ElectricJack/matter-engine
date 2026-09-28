# PomProof — reading ground POM off a sphere

A world whose only purpose is to make parallax occlusion mapping legible. Three
hemispherical domes (radius 15, 40, 80 m) on a dead-flat plane, wearing **one**
detail tileset — `ForestFloor` — with no classifier, no blend and no second
material anywhere. Nothing in this world varies except surface orientation.

    MATTER_WORLD=PomProof ./build/windows/editor.exe

Cold bake is about 20 s (the `ForestFloor` `.gtex` settle is ~9 s of it).

---

## Why a sphere

POM fails as a function of the angle between the view ray and the surface. On
StreamMountain that angle is inseparable from everything else: a steep face is
also a different tileset, at a different distance, next to a blend boundary. A
hemisphere presents **every** angle from 0° at the crown to 90° at the rim,
continuously, on one object, at one distance, in one material. So an
angle-dependent defect has nowhere to hide, and — because the shape is an exact
closed form — the angle at any point can be *computed* rather than guessed:

    y(d) = 1 + R * sqrt(max(0, 1 - (d/R)^2))       d = distance from the axis

For an eye at `E` and a dome centred at `C` with radius `R`, the **silhouette**
(incidence exactly 90°, where the ray goes tangent) is the circle at distance
`R²/L` from `C` along the `C→E` axis with radius `R·sqrt(1 - R²/L²)`, where
`L = |E - C|`. Everything between the near rim and that circle sweeps
monotonically from face-on to tangent.

The flat ground is the same ruler for free: from an eye at height `h`, flat
ground at horizontal distance `d` sits at incidence `atan(d/h)`.

## What to look at

Spawn puts you ~12 m from the 15 m dome with flat ground under you and running
to the horizon. In that one frame:

* **the near dome flank** sweeps face-on (lower right, crisp pebbles) to tangent
  (the silhouette on the left, where the pebbles smear into streaks);
* **the flat ground** does the same thing with distance — crisp underfoot,
  dissolving into a structureless band toward the horizon;
* both are the same texture, so any difference between them is about geometry,
  not material.

Other cameras are listed in a comment at the top of `PomProof.js` (the world
schema carries one camera and no fov). Drive them live with
`cam ex ey ez tx ty tz` over `MATTER_CMD_FIFO`.

## Knobs that matter

All live — no `reload` needed — under **Ground POM** in the properties panel, or
over the FIFO:

| key | default | range | what it does |
|---|---|---|---|
| `render.pom.relief_cap_m` | 0.352 | 0–0.5 | caps how deep the relief is taken to be. `relief = min(atlas h_range, this)`. **This is the "displacement" slider** — sweep it and watch the rocks slide inward. Useful travel is 0 → ~0.35; see below. |
| `render.pom.max_march_m` | 1.59 | 0.1–2.0 | hard cap on how far the ray may travel. This is the grazing-smear knob; see below. |
| `render.pom.steps` | 30 | 4–64 | linear march steps before the 4 refine iterations. Fades to a floor of 8 by `max_distance_m`. |
| `render.pom.enabled` | true | — | soft disable (uploads 0 steps); the flat Wang tile still shows. |

### How much travel the relief cap actually has

Measured at the close flank camera, each step against the next:

| step | pixels changed | mean \|Δ\| |
|---|---|---|
| 0.05 → 0.352 | 72.6 % | 27.4 |
| 0.352 → 0.50 | 7.7 % | 0.4 |

So the cap does almost all of its work below ~0.35 m and is close to saturated
there: pushing it to the 0.50 ceiling moves a twelfth of the frame by less than
half a code value. `relief = min(atlas h_range, relief_cap_m)`, so this says
ForestFloor's baked `h_range` sits **just above 0.352** — above it, because 0.50
and 0.352 do render differently, but not far above. *I could not determine the
exact `h_range`*: nothing logs it and the slider cannot go past 0.5. Treat 0.352
as effectively "full relief" for this atlas, and read the cap as a 0 → 0.35
control.

`MATTER_VT_WARP=0` reverts the march's addressing to the shipped world-XZ form.
It is read **once per process** into a function-local static, so warp on and warp
off need two separate launches — it cannot be toggled over the FIFO.

For A/B pairs use `MATTER_DISABLE_VK_RT=1` on both sides. With RT off this world
is bit-deterministic: setting a knob and setting it back reproduces the previous
PNG byte for byte (verified twice).

---

## The measurement this world exists for

`tileset_pom_march` (`MatterEngine3/shaders_vk/tileset_common.glsl`) computes

    cos_theta = max(abs(dot(ray_dir, plane_n)), 0.08)
    march_len = min(relief / cos_theta, max_march_m)

so the cap engages exactly when

    cos_theta  <  relief / max_march_m

Above that contour the march always reaches full relief depth and POM is exact.
Below it the ray stops short, and wherever it has not yet crossed the relief the
shader returns the **capped end point** — a texel up to `max_march_m` away
laterally, which on a 2 m Wang tile is most of a tile. That is the smear.

**This gives a way to locate incidence contours in image space with no camera
matrix at all**: the set of pixels that change when `max_march_m` moves from `a`
to `b` is exactly the set whose `cos_theta` lies between `relief/b` and
`relief/a`. Sweep the cap and the changed-pixel mask paints the contours onto
the picture.

### Measured, at the close flank camera (`cam 37 6 0 53 15 0`)

Fraction of fragments whose march outcome changes when `max_march_m` is raised
from the default 1.59 m to 2.00 m — i.e. fragments where 1.59 m of march was not
enough — against the incidence angle at that point:

| incidence | fragments the cap decided |
|---|---|
| < 75° | 0.00 % |
| 75–77° | 1.3 % |
| 77–79° | 5.1 % |
| 79–83° | 7.1 % |
| 83–87° | 17.3 % |
| 87–90° | 18.1 % |

**Onset is ~75°, material by ~79°, and it never touches anything flatter.** The
formal contour for the defaults is `acos(0.352/1.59)` = **77.2°**; first measured
change is a couple of degrees shallower, which is the expected slop between the
analytic sphere normal and the interpolated mesh normal the shader actually gets
(see "what the mesh can hold" below).

### How `max_march_m` moves the threshold

`θ_onset = acos(relief / max_march_m)`, confirmed by the ladder:

| `max_march_m` | θ_onset | lands at image row |
|---|---|---|
| 0.36 | 12.1° | 720 (the whole frame) |
| 0.45 | 38.5° | 424 |
| 0.55 | 50.2° | 296 |
| 0.70 | 59.8° | 200 |
| 0.90 | 67.0° | 137 |
| 1.15 | 72.2° | 98 |
| **1.59** | **77.2°** | **67** |
| 2.00 | 79.9° | 54 |

The curve is `acos(k/m)`, so it is brutally flat at the top: going from 1.59 m to
the slider's 2.00 m ceiling (+26 % of march) buys **2.6°**. Pushing the onset to
85° would need `max_march_m = 0.352/cos 85° = 4.04 m`, twice the ceiling — and a
4 m lateral march samples two whole Wang tiles away, which is not less smeared,
only differently smeared.

The knob that actually moves the threshold the useful way is `relief_cap_m`,
because it moves the numerator: at `relief_cap_m = 0.12` with the default march,
onset is **85.7°**. That is the real trade — **grazing correctness is bought with
parallax depth**, one for one. It is a trade, not a fix.

---

## What the mesh can hold

Terrain LOD 5 is a 2 m voxel lattice, and that bounds how steep a *shading*
normal the domes can present. `gbuffer.frag` hands the march
`plane_n = normalize(in_normal)` — the interpolated gradient normal — so this is
the number that matters, not the face normal. Measured over the meshed caps
(area-weighted, `mesh_sector` rung 0):

| dome | steepest face normal | steepest gradient normal | area past 70° |
|---|---|---|---|
| A, R = 15 m | 82.8° | 70.8° | 1.4 % |
| B, R = 40 m | 88.5° | 78.8° | 30.3 % |
| C, R = 80 m | 89.8° | 82.2° | 31.3 % |

Bigger domes resolve the rim better, which is the other reason there are three of
them. Note that a shading normal capped at 78.8° does **not** cap the incidence:
`cos_theta` still reaches 0.0003 (89.98°) on dome B from the spawn camera,
because the ray sweeps through the normal. Measured `cos_theta` on the visible
cap of dome B from `eye (37, 6, 0)`: 0.837 at the foot falling monotonically to
0.009 at the silhouette, with 51 % of the visible area below the default
truncation contour.

## Files

* `PomProof.js` — the world.
* The domes come from `dome(cx, cz, radius[, height])` in
  `MatterEngine3/src/world_base.js.h`, built on the `worldX()` / `worldZ()`
  coordinate nodes added to the field tape in the same commit. Before that the
  field DSL had no coordinate access at all — every op was translation-covariant
  noise, so an authored shape at an authored place was not expressible.
