// BrickProof — a masonry running bond authored as a DIAGNOSTIC INSTRUMENT for
// ground POM. Not a facade material: every number below is chosen to make a
// parallax failure visible to the naked eye, and several of them are
// deliberately unrealistic.
//
// WHY A BRICK GRID AND NOT NOISE
//
// Every other detail tileset in this project (ForestFloor, the four Alpine
// sets, ScreeDetail) is a spectral sum plus scattered litter — i.e. a
// stochastic field. Stochastic fields HIDE the four failures this texture
// exists to expose:
//
//   1. SHEAR AND SMEAR. A pebble field can be smeared halfway across a tile
//      and still look like a pebble field; there is no "correct" position for
//      any pebble, so nothing looks wrong. A brick course is a straight line
//      with a known direction. If it bends, wobbles, tears or shears, that is
//      unmistakable at a glance and needs no reference image.
//
//   2. OCCLUSION. Flat faces separated by deep, sharp-walled grooves give the
//      strongest possible occlusion cue: at a grazing angle the near lip of a
//      brick MUST hide the groove behind it. That is the single clearest read
//      on whether parallax is working. Where it fails you see the groove
//      smear THROUGH the brick face instead of being occluded by it — a
//      failure mode that is literally invisible on noise.
//
//   3. PARAMETERISATION. This is the property that motivated the texture. On a
//      dome flank the VT warp field re-parameterises the surface, and a
//      regular lattice draws that re-parameterisation directly: curved
//      courses, varying brick size and shear ARE the warp, made visible.
//      Noise cannot show this because noise has no lattice to distort.
//
//   4. KNOWN DEPTH. The groove depth is authored (GROOVE_DEPTH below), so the
//      apparent displacement can be checked against a number rather than
//      merely observed to be "present". ForestFloor's README could not
//      determine its own baked h_range; this tileset's is predictable in
//      closed form (see THE HEIGHT BUDGET).
//
// ---------------------------------------------------------------------------
// THE HARD CONSTRAINT: THE BASE IS 64 SAMPLES, NOT 1024
//
// `this.base(fn, mat)` is sampled on a fixed BaseField::kSamplesPerTile = 64
// grid (MatterEngine3/src/tileset_spec.h), regardless of texelsPerMeter. The
// callback is invoked at exactly x, z = i * (size/64). Those samples become
// heightfield VERTICES in the torus BVH (tileset_torus_bvh.cpp tessellates
// 2 triangles per cell), and the 512 texels/m atlas is then ray-cast down onto
// that mesh. So the atlas resolves at 512 texels/m but the GEOMETRY it records
// is piecewise-linear on a 2.0/64 = 3.125 cm lattice.
//
// Everything about the layout below follows from that one number:
//
//   * A groove WALL cannot be steeper than one cell. 0.12 m over 3.125 cm is
//     a 75.4-degree wall — the sharpest this representation can hold.
//   * A FLAT-BOTTOMED groove needs at least two adjacent samples at floor
//     depth, so the narrowest flat-bottomed groove is 3 cells (9.375 cm)
//     across: one cell of wall, one cell of floor, one cell of wall.
//   * Therefore every joint, course and brick boundary is placed on an
//     INTEGER number of cells. Nothing is allowed to land between samples,
//     because a boundary that falls between samples is silently rounded and
//     the bond loses phase.
//
// NO CHAMFER, and that is a considered choice rather than an omission. A
// chamfer would have to consume a whole 3.125 cm cell — it would take the wall
// run from one cell to two, halving the wall slope from 75.4 to 62.4 degrees
// and widening the groove from 3 cells to 5. On this grid there is no such
// thing as a SMALL chamfer; the smallest expressible one is a major softening
// of exactly the occlusion edge the texture exists to test. Sharp is more
// diagnostic than pretty.
//
// ---------------------------------------------------------------------------
// THE LAYOUT, AND WHY IT WRAPS
//
// A tileset atlas is toroidal: the tile must join itself seamlessly at 2 m in
// both axes or a brick-grid texture prints a visible fault line every 2 m,
// which would wreck the instrument (a periodic tear is exactly the artefact
// we are hunting for). The wrap here is STRUCTURAL, not approximate — every
// term in the height function is a function of integer indices reduced modulo
// a divisor of 64, so h(x + 2, z) == h(x, z) and h(x, z + 2) == h(x, z)
// exactly, in floating point, with no tolerance involved:
//
//   * 8 courses of 8 cells   -> 64 cells = 2.0 m.   8 divides 64.
//   * 4 bricks of 16 cells   -> 64 cells = 2.0 m.  16 divides 64.
//   * running-bond offset    -> 8 cells (half of 16), applied on ODD courses.
//     The offset pattern has period 2 courses = 16 cells, and 64/16 = 4 whole
//     cycles, so the bond returns to phase at the tile edge. This is why the
//     course count must be EVEN: with an odd count the last course would butt
//     against the first at the wrong offset and print a running seam.
//   * the per-brick height jitter hashes (course mod 8, brick mod 4), so it
//     wraps with the bond rather than fighting it.
//
// One further nicety falls out of the half-cell lattice below: the joints land
// ON the tile boundary in both axes (samples 63 and 0 are both groove floor),
// so the seam is buried inside a groove rather than running across a brick
// face. Even if the wrap were imperfect it would be hidden; it is not
// imperfect, but the belt and braces cost nothing.
//
// THE HALF-CELL LATTICE. Joint centre lines sit HALFWAY BETWEEN samples, not
// on them. If a joint were centred on a sample there would be exactly one
// sample at floor depth and the groove would be a V, not a flat-bottomed
// channel. Centring between samples puts two samples (at +/- half a cell) at
// floor depth, which is a one-cell flat floor with one-cell walls either side.
// That is what `u = s + 0.5` is doing, and it is the whole reason the grooves
// have bottoms.
//
// ---------------------------------------------------------------------------
// THE HEIGHT BUDGET (and why it is worth writing down)
//
// The bake's compute_height_range (render/tileset_bake_vk.cpp) takes the
// min/max over the base samples and then pads: hmax += 0.05, hmin -= 0.05.
// With brick tops in [-0.02, 0] and groove floors at -0.12:
//
//   height_max = +0.05    height_min = -0.17    h_range = 0.22 m
//
// The shader (tileset_common.glsl) then anchors relief at the DATUM = height_max
// and clamps: raw = absolute_height - height_max, clamped to [-relief, 0],
// with relief = min(h_range, render.pom.relief_cap_m). So:
//
//   brick tops   -> raw in [-0.07, -0.05]
//   groove floor -> raw  = -0.17
//
// MEASURED, by sweeping render.pom.relief_cap_m over 0.05 .. 0.50 at a fixed
// camera with RT off (frames are bit-deterministic, so "no change" means
// pixel-identical, not "looks the same"):
//
//   * h_range is 0.220 EXACTLY, confirmed against the baked atlas: the dumped
//     height channel spans texels [58, 197]/255, which decodes to
//     [-0.1200, -0.0000] m — the authored groove depth and the un-jittered
//     brick plane, to four decimal places. ForestFloor could not offer this:
//     its README records that its h_range sits somewhere just above 0.352 and
//     could not be pinned down, because the slider tops out at 0.5.
//
//   * The picture saturates at relief_cap_m = 0.22, i.e. at h_range — every
//     capture from 0.22 up to the 0.50 ceiling is pixel-identical.
//
//     I first predicted 0.17 (= GROOVE_DEPTH + the 0.05 padding), reasoning
//     that clamp(raw, -relief, 0) can do nothing once relief exceeds the
//     deepest raw value of -0.17. That reasoning is right but incomplete, and
//     the extra 0.05 m of travel is worth understanding: `relief` is used
//     TWICE in tileset_pom_march. It bounds the clamp, and it is also the
//     numerator of march_len = min(relief / cos_theta, max_march_m). So above
//     0.17 the height field is frozen but the march is still getting longer,
//     which changes the STEP SIZE (march_len / steps) and hence where the
//     discrete march detects each groove edge. Measured: the 0.17 -> 0.18
//     step moves 53.6% of the frame, and that change vanishes above ~83.5 deg
//     — exactly where max_march_m takes over as the binding term and relief
//     stops setting march_len. Below 0.17 both mechanisms move together.
//
//   * Below relief_cap_m = 0.05 the ENTIRE tile clamps to a single value and
//     the surface goes perfectly flat — a free "POM fully off" control that
//     still shows the albedo and AO of the brick pattern.
//
// The smear-onset contour acos(relief / max_march_m) uses relief = 0.22 at
// default cap, giving 82.0 degrees rather than ForestFloor's 77.2. To probe
// the formula at 77.2 degrees, set render.pom.max_march_m = 0.99
// (0.22 / cos 77.2 = 0.994).
//
// BEWARE what that contour does and does not predict. Measured over a 7-rung
// max_march_m ladder, it locates where the render CHANGES to within about a
// degree. It does NOT locate visible smear: the un-marched remainder is zero
// AT the contour and grows continuously, so the lateral error only reaches
// texture scale (a 0.25 m course pitch) several degrees further up. At the
// shipped default that puts the whole cap-affected region in a ~14-pixel
// sliver at the silhouette. See PomProofBrick.README.md.
//
// ---------------------------------------------------------------------------
// ONE MATERIAL, NO PER-BRICK COLOUR — a limitation of the API, not a choice.
//
// `base(fn, mat)` takes a height callback and a SINGLE material constant
// (dsl_bindings.cpp j_ts_base: `JS_ToUint32(c, &mat, a[1])`), and the atlas
// albedo is written as `mat_albedo(mid)` from a per-triangle material id
// (tileset_bake_primary.comp) — every triangle of the base carries that one
// id. Per-brick albedo is therefore NOT expressible through base(). The only
// route to colour variation in a tileset is child geometry (layer/dropChild),
// and both of those go through the physics settle, which tumbles what it
// places — useless for laying a precise bond.
//
// This turns out to be a virtue here. With a uniform albedo, EVERY visible
// feature of this texture is relief: the height channel, the baked normals and
// the baked AO in the grooves. There is no colour cue that could be mistaken
// for parallax, so what the eye reads is exactly what POM computed.
//
// It also makes the near/far albedo handling a no-op. The near band modulates
// the live detail tap by atlas_albedo / mean_albedo (the "modulate not replace"
// path, tileset_common.glsl). A single-material base has atlas_albedo ==
// mean_albedo at every texel, so that ratio is exactly 1.0 everywhere and the
// world material's albedo passes through untouched — no near/far step to be
// mistaken for a defect.
//
// MAT.dirt, matching ForestFloor's base material exactly, and deliberately so:
// it makes BrickProof differ from ForestFloor in NOTHING BUT THE HEIGHT FIELD.
// Same material id, same albedo, same roughness, same tile size, same
// texels/m. So PomProof vs PomProofBrick at a shared camera is a clean
// single-variable comparison of "stochastic relief" against "regular relief",
// which is the whole argument for building this texture.
// ---------------------------------------------------------------------------

class BrickProof extends Tileset {
  // No child modules. Unlike every other tileset here, nothing is scattered:
  // scattered litter would sit in front of the surface under test and, being
  // per-tile random, would reintroduce exactly the stochastic camouflage this
  // texture exists to remove. All 16 Wang tiles are consequently identical,
  // which makes the world a perfectly periodic brick wall — the ideal
  // parameterisation visualiser, since any deviation from perfect periodicity
  // in the final image is the warp or the march, never the texture.
  static requires = [];

  build() {
    const SIZE = 2.0;              // metres — matches ForestFloor and the Alpine sets
    this.tile({ size: SIZE, texelsPerMeter: 512, seed: 20260802 });

    // --- the sample lattice (mirrors BaseField::kSamplesPerTile) ------------
    const N    = 64;
    const CELL = SIZE / N;         // 0.03125 m

    // --- bond geometry, in CELLS so nothing can land off-lattice -----------
    const COURSES      = 8;        // even, divides N -> bond returns to phase
    const BRICKS       = 4;        // divides N
    const COURSE_CELLS = N / COURSES;      // 8  -> 0.25 m course pitch
    const BRICK_CELLS  = N / BRICKS;       // 16 -> 0.50 m brick pitch
    const OFFSET_CELLS = BRICK_CELLS / 2;  // 8  -> 0.25 m running-bond offset

    // Joint half-width as a step threshold in cells. Samples sit at half-cell
    // distances (0.5, 1.5, 2.5, ...) from a joint centre, so a threshold of
    // 1.0 selects exactly the two samples at +/-0.5 and rejects those at
    // +/-1.5 — a one-cell flat floor with a full half-cell of margin on either
    // side of the comparison, i.e. no floating-point knife edge.
    // Resulting joint: 3 cells = 9.375 cm across, 3.125 cm of flat floor.
    const JOINT_HALF = 1.0;

    // Deliberately exaggerated: roughly 12x a real 10 mm mortar joint's depth.
    // Sits in the middle of the useful POM band (the relief cap defaults to
    // 0.352 m) and gives a groove aspect of 0.12 / 0.09375 = 1.28 : 1, so the
    // floor of a groove passes out of sight about 38 degrees off face-on.
    // That early, sharp cut-off is the occlusion read.
    const GROOVE_DEPTH = 0.12;

    // Per-brick settle, DOWNWARD ONLY. Downward rather than symmetric so the
    // un-jittered plane is the true maximum of the field: that keeps the bake
    // datum tight against the brick tops (see THE HEIGHT BUDGET) and leaves a
    // crisp common lip height for the bricks that draw a zero. 2 cm is enough
    // to resolve individual bricks against their neighbours without breaking
    // the "flat faces" property that the occlusion cue depends on.
    const JITTER = 0.02;

    // Small integer avalanche hash -> [0, 1). Used on (course, brick) only, so
    // it inherits the bond's periodicity and cannot break the wrap.
    const hash01 = (a, b) => {
      let h = (Math.imul(a, 73856093) ^ Math.imul(b, 19349663)) >>> 0;
      h = Math.imul(h ^ (h >>> 15), 0x2c1b3c6d) >>> 0;
      h = Math.imul(h ^ (h >>> 13), 0x297a2d39) >>> 0;
      return (h >>> 8) / 16777216;
    };

    // Distance from `q` to the nearest multiple of `period`, in cells.
    const distToLattice = (q, period) => {
      const m = ((q % period) + period) % period;
      return Math.min(m, period - m);
    };

    this.base((x, z) => {
      // Recover the integer sample index. base() is called at exactly
      // i * CELL, so round() is exact recovery, not a tolerance.
      const s = Math.round(x / CELL);
      const t = Math.round(z / CELL);

      // Half-cell-shifted lattice coordinates: joint centre lines fall at
      // integer multiples of the pitch in (u, v), which is HALFWAY BETWEEN
      // samples in (s, t). See THE HALF-CELL LATTICE above.
      const u = s + 0.5;
      const v = t + 0.5;

      // Bed joints (horizontal, between courses).
      if (distToLattice(v, COURSE_CELLS) < JOINT_HALF) return -GROOVE_DEPTH;

      // Which course, and hence which way the bond is offset. v is never an
      // exact multiple of COURSE_CELLS (it is always a half-integer), so the
      // floor() below never lands on a boundary. The modulo is written the
      // positive way (JS `%` keeps the sign of the dividend) so the function is
      // periodic for ALL integer s, t rather than merely for the [0, 64) window
      // the engine happens to sample — otherwise the wrap would be true by
      // accident of the call range instead of by construction.
      const course = ((Math.floor(v / COURSE_CELLS) % COURSES) + COURSES) % COURSES;
      const uo = u - ((course % 2) ? OFFSET_CELLS : 0);

      // Head joints (vertical, between bricks in a course).
      if (distToLattice(uo, BRICK_CELLS) < JOINT_HALF) return -GROOVE_DEPTH;

      // Brick face. Positive modulo: uo goes negative on offset courses.
      const brick = ((Math.floor(uo / BRICK_CELLS) % BRICKS) + BRICKS) % BRICKS;
      return -hash01(course, brick) * JITTER;
    }, MAT.dirt);
  }
}
