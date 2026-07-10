# Particle-Flow Tree Generation — Design

**Date:** 2026-07-09
**Status:** Approved design, pending implementation plan
**Replaces:** L-system trunk/branch generation in `MatterEngine3/examples/world_demo/schemas/Tree.js`

## Goal

Replace the L-system-based trunk/branch generation in Tree.js with a particle-flow
growth system that blends into space colonization. Particles ARE the growth process:
strands emitted at the tree base rise under steering fields, their traced paths become
the skeleton, and the union of swept strand paths IS the trunk surface (Holton-style
strand model — thickness emerges from bundle cross-section, fluting and bark grain
emerge from strand layering). L-system twigs and leaves attach along branches.

The heavy lifting runs in C++ as generic, reusable building blocks; all tree-specific
behavior is configured and scripted in the DSL. The particle kernel is a general agent
system (this is its first application — future uses include leaf scatter, sand-castle
drips, roots, vines, river networks).

### Hard requirements

- **Strict monotonic accretion.** Geometry present at age T never moves or retracts at
  age T+1. Thickening happens by new strands flowing over the outside of existing wood.
  Enforced structurally: the deposited point set and the recorded PathSet are
  append-only. Enables age-scrubbing and watchable growth.
- **Determinism.** Same seed + config + tick count ⇒ bit-identical output.
- **Steppable simulation.** Age is a static part parameter today; the sim object
  supports incremental `run()` calls so a future live-growth mode (watching parts bake
  during world install) needs no API change. **No bake-machinery changes now** — Phase C
  owns bake-order rework.
- **DSL-first iteration.** Aesthetic tuning happens by editing Tree.js and reloading the
  world, not by rebuilding C++.

## Architecture

Small family of C++ blocks with typed data contracts, wired and configured from the
DSL. Blocks connect through data types, not through each other.

### Contract types (`ParticleFlowLib/include/particle_flow.h`)

- **`pf_particle_frame`** — SoA views of live particle state at tick T: positions,
  velocities, named scalar attribute channels, alive flags, born/died-this-tick lists.
- **`pf_path_set`** — append-only set of polylines with per-vertex attribute channels
  (radius, age, …) and stable path ids. Append-only is load-bearing: it is the
  monotonic-accretion guarantee, and downstream consumers can process incrementally.

### Blocks

- **Tick-domain blocks** implement a small observer interface
  (`on_tick(const pf_particle_frame&)`). The sim owns an observer list; per-tick data
  flows C++→C++ with zero JS involvement. JS taps in only via the scheduled `onTick`
  callback.
- **Set-domain blocks** are pull-based transforms over a `pf_path_set` (consume what
  appended since last call). Works one-shot at bake today, incrementally for live
  growth tomorrow.

Anything producing a particle-frame stream can drive the recorder; anything producing a
PathSet can drive geometry sinks or future PathSet→PathSet transforms (relaxers,
bundlers, pruners).

### Project layout

New sibling lib project **`ParticleFlowLib/`** (monorepo pattern: `include/`, `src/`,
own Makefile, headless test suite, no engine dependency). One project with hard module
boundaries inside; blocks that later earn independence can be extracted then.

- `pf_sim` — the particle kernel
- `pf_path_recorder` — trajectory-recording observer

**MatterEngine3** links the lib; DSL bindings live in `dsl_bindings.cpp` via
`install_bindings()`. **`shared-lib/strands.js`** provides thin tree-flavored
conveniences (crown attractor-cloud generators, emission helpers, twig placement).
Nothing tree-specific exists in C++.

### Explicitly rejected

Box3d-backed growth particles: rigid-body impulse solving is the wrong shape for
kinematic steering + deposition (determinism risk, broadphase churn at thousands of
frozen bodies, ForestFloor settle-scale data point). Box3d-in-the-DSL may be worth
doing for settling use-cases someday; it is not part of this design.

## The particle kernel (`pf_sim`)

### State (SoA, Float32Array-compatible buffers)

- `pos[3]`, `vel[3]` — **velocity-canonical**: velocity is the general representation
  (supports ballistic uses: gravity, wind, drips). Heading/speed are derived.
- N user-declared named scalar attribute channels (Tree.js declares `radius`); C++
  treats them as opaque floats that fields and JS can read/write.
- `alive` flags; slot reuse via free list, but particle ids are stable and never
  recycled within a sim so paths stay unambiguous.

### Step loop (fixed dt, deterministic order = ascending id)

1. **Emission** — declarative emitters (point/disc/ring source; rate per tick; initial
   velocity + jitter; initial attribute values) plus anything JS emitted in the last
   callback.
2. **Force fields** — fields in `force` mode output accelerations: `vel += F * dt`.
3. **Steering** — fields in `steer` mode output desired directions; the integrator
   rotates `vel` toward the weighted steer target by at most `maxTurnRate` per tick,
   magnitude preserved. Bounded curvature independent of speed — this is what makes
   strands read as grown wood.
4. **Speed regulation** (optional) — `|vel|` relaxes toward `speedTarget` at
   `speedRelax`. Strands set it (constant growth rate); ballistic uses don't.
5. **Integrate** — `pos += vel * dt`.
6. **Deposit** — each particle appends its position to the deposited point set
   (spatial-hashed) every `depositEvery` distance traveled.
7. **Kill rules** — max age, JS-set kill flag; attractor-capture kill (below).
8. **Notify observers**, then the JS `onTick` callback if scheduled this tick.

### Field primitives

Each field has: application mode (`force` | `steer`), weight, falloff radius where
applicable, and an optional **axial fade** (`fade: { axis, from, to }` — weight ramps
to zero across a spatial band along an axis). Weights are mutable from JS per tick.

| Field      | Behavior |
|------------|----------|
| `bias`     | constant direction. Steer mode: growth direction (trunk rise). Force mode: gravity/wind. |
| `curlNoise`| divergence-free noise steering/force (organic wander; own seed). |
| `adhere`   | steer toward the average of nearby **deposited** points minus a surface-normal offset — new strands flow over the outside of existing wood (thickening, root flare, bark grain). |
| `attract`  | space colonization: steer toward nearest unconsumed attractor within `influence` radius; attractor consumed when any particle enters `killRadius`. |
| `separate` | push away from nearby **live** particles (anti-collapse; the adhere↔separate tension is the fluting knob). |
| `drag`     | force mode: `-k · vel` (for ballistic uses). |

### Attractor cloud

Passed from JS as a flat Float32Array of points (crown-shape authoring belongs in
script). Sim tracks consumed flags; JS can query remaining count (natural termination
signal) and append points mid-run.

### Spatial hash

One uniform grid, two point categories (live / deposited). Cell size = max query
radius. Live set rebuilt per tick; deposited set inserted incrementally. All neighbor
queries (adhere, separate, attract candidates, surface-normal estimates) go through
it; nothing is O(n²).

Budget sanity: ~300 strands × ~2000 ticks × ~30 neighbor lookups ≈ 20M hash queries in
C++ — tens of milliseconds. JS boundary ≈ number of scheduled onTick calls. Bake cost
remains dominated by voxelization/retopo, not the sim.

### Concurrency & determinism

- The kernel is **instance-contained**: all state (SoA buffers, spatial hash, RNG,
  deposited set, attractors) lives in the sim object. **No statics, no globals, no
  shared caches** (the autoremesher/TBB singleton fragility is the cautionary tale).
- Each sim instance is single-threaded internally and independently deterministic;
  N sims run concurrently on N bake workers with zero coordination — this happens
  today, since Phase B async bake runs part bakes on worker threads.
- Intra-sim parallelism is out of scope (would require deterministic reduction; the
  API would not change if it were ever added).
- RNG: xoshiro owned by the sim, seeded from config (part seed). Never wall-clock,
  never `Math.random` inside the kernel. JS callbacks are part of the recipe and must
  use the part-seeded DSL RNG (already the norm).

## DSL binding surface

Installed in `install_bindings()` (dsl_bindings.cpp). Objects are opaque handles with
finalizers, valid only during the owning part bake (same lifetime rules as sessions).

```js
const sim = engine.particleSim({
  seed,                      // from part params — deterministic
  dt, maxTurnRate, speedTarget, speedRelax,
  attributes: ['radius'],    // named scalar channels
  emit:   { shape:'disc', center, radius, axis, rate, vel0, jitter, attrs:{radius:...} },
  fields: [
    { type:'bias',    mode:'steer', dir:[0,1,0], weight:1.0,
      fade:{ axis:[0,1,0], from:h0, to:h1 } },
    { type:'adhere',  mode:'steer', weight:.., radius:.., surfaceOffset:.. },
    { type:'separate',mode:'steer', weight:.., radius:.. },
    { type:'curl',    mode:'steer', weight:.., scale:.., seed:.. },
    { type:'attract', mode:'steer', weight:.., influence:.., killRadius:.. },
    { type:'drag',    mode:'force', k:.. },
  ],
  depositEvery: VOX * 0.5,
  maxAge: ..,
  maxParticles: ..,          // hard cap, default ~16k
});

const rec = engine.pathRecorder({ minSegment: VOX * 0.5 });  // decimation
sim.attach(rec);
sim.setAttractors(cloudF32);           // Float32Array xyz triplets
sim.run(nTicks, { every: 8, onTick(view, t) { ... } });      // incremental; callable repeatedly
```

- **onTick view** — zero-copy Float32Array/Uint8Array aliases over the SoA buffers
  (QuickJS external ArrayBuffers), **valid only during the callback** (detached on
  return; stale access throws). Surface: `view.count`, `view.pos`, `view.vel`,
  `view.attr(name)`, `view.alive`, `view.emit({...})`, `view.kill(i)`.
- **Sim controls/queries**: `sim.setFieldWeight(index, w)`, `sim.attractorsRemaining()`,
  `sim.surfaceNormal(p)` (deposited-set neighborhood normal), `sim.depositedCount()`.
- **Recorder queries**: `rec.pathCount()`, `rec.path(i)` →
  `{ points: Float32Array, attr(name), endDir, length }`.

### Geometry sink — `this.paths(rec, opts)`

- Valid inside a **voxel session**: iterates every path in C++, stamping each
  consecutive vertex pair as a varying-radius capsule (radius from the named channel,
  floored at `opts.minRadius`), blended with the session's current smooth-k. One JS
  call stamps the whole tree.
- Respects the current transform stack like every other geometry verb.
- **Session-polymorphic contract**: mesh session → tube skinning, specced but not
  implemented now. Errors with a clear "not yet supported in mesh sessions" message —
  never a silent no-op (the MSL `simplify(MeshIndexed)` lesson).
- `opts.filter` — optional JS predicate on path index for stamping subsets
  (trunk vs twig consumers).
- `opts.radiusChannel`, `opts.minRadius`.

## The Tree.js growth recipe (all JS — the iteration surface)

### Setup

- Attractor cloud: JS scatters ~200–500 seeded points in a crown volume
  (ellipsoid/cone above trunk height) via `shared-lib/strands.js` helpers.
- Emitter: disc/ring at the base. **Age = emission budget + tick budget** (static part
  param). Initial velocity up with slight outward jitter.
- Attribute `radius`: per-strand, roughly constant ~1–2× VOX with tip taper. Trunk
  thickness emerges from bundle cross-section (Holton), not fat radii.

### Field stack

| Field | Mode | Role |
|-------|------|------|
| `bias` (up) | steer, axial fade across the crown transition band | trunk rise |
| `adhere` | steer | flow over deposited wood: thickening, root flare, bark grain |
| `separate` | steer | anti-collapse; fluting knob (vs adhere) |
| `curlNoise` | steer, low weight | organic wander |
| `attract` | steer, influence limited to near-cloud | space colonization takeover |

The trunk→crown handover is **spatial, not temporal**: up-bias fades over a height
band while attractors only capture strands entering their influence radius.
Late-emitted strands still get trunk behavior low and colonization high — no
per-particle scripting needed.

**Branching is emergent**: attractor clusters pull strand subsets apart; each
peeled-off bundle is a branch with thickness ∝ strand count (Holton's rule for free).

**Termination**: JS polls `attractorsRemaining()` in `onTick`; when consumption
plateaus, stop running. Max-age kill catches stragglers.

### Twig/leaf placement (post-sim)

Sample candidate points along every crown path's vertices, parameterized by arclength
fraction `t` toward the tip; accept with probability ∝ `t^k` (density favors branch
ends; `k` tunable; seeded RNG). Each accepted point is pushed to the surface and
oriented along the **local surface normal** from `sim.surfaceNormal(p)`
(`normalize(p − avgNearbyDeposited(p))` — consistent with the actual accreted wood,
no engine-side SDF query needed). Twig orientation = surface normal blended toward the
branch growth direction as `t→1`, so tip twigs continue the colonization direction and
mid-branch twigs sprout outward. Twigs are small L-systems (existing `lsystem.js`),
stamped in the same voxel session; leaves hang off twigs as today.

### Geometry assembly

```js
this.beginModifier();
this.beginVoxels(VOX);
this.paths(rec, { radiusChannel: 'radius', minRadius: VOX });
/* twig/leaf stamping */
this.endVoxels();
this.endModifier(stack);   // existing simplify/smooth/retopo stack unchanged
```

### Aging

Age scales strand count + ticks. Because deposition is append-only and new strands
adhere to the outside, an older tree is strictly the younger tree plus material —
age-scrub and watch-it-grow are structurally guaranteed. Live growth later = the
engine holds the sim and calls `run()` incrementally; the Tree.js recipe is unchanged.

## Error handling

- **Config validation at construction**: unknown field type, missing required param,
  bad attribute name → JS exception naming the offending key. Fail loud at
  `engine.particleSim(...)`, not mid-run.
- **Runtime guards**: `run()` after handle finalization throws; onTick views are
  detached on callback return (stale access throws, never reads freed memory);
  `this.paths()` outside a voxel session → clear session-polymorphism error.
- **Budget**: `run()` checks the engine's existing script `time_budget_ms` between
  ticks so a runaway sim aborts the part bake cleanly; `maxParticles` hard cap
  prevents emit-loop OOM.
- **NaN guard**: integrator kills any particle whose position goes non-finite (debug
  log line, not an assert — the Windows release build carries `-DNDEBUG`).

## Testing

- **`ParticleFlowLib/tests/`** (headless, no GL): determinism (two sims, same seed,
  byte-compare PathSets); incremental-run equivalence (`run(N+M)` ≡ `run(N); run(M)`);
  field behaviors (force-mode bias produces a gravity parabola; turn-rate clamp bounds
  curvature; attractor consume/kill; adhere pulls toward the deposited set);
  spatial-hash correctness vs brute force; emission/kill/slot-reuse with stable ids.
- **MatterEngine3 binding tests** (existing `tests/` pattern): handle lifecycle +
  finalizers; zero-copy view aliasing (write via view, read back from C++); stale-view
  throw; `this.paths()` stamps expected SDF (small sim → voxel session → non-empty
  mesh with plausible bounds); config-error messages.
- **Visual gate**: Tree.js on Meadow via `viewer_shots.sh` FIFO flow
  (`GALLIUM_DRIVER=d3d12`), shots at 2–3 ages of the same seed to eyeball monotonic
  growth. Look-tuning is manual — that is the iteration loop, not a test.

## Future work (out of scope)

- Mesh-session tube skinning for `this.paths()` (contract specced; voxel-only now).
- Collision/settle story for ballistic uses (ground plane, deposited-set contact).
- Engine-held live sims stepped per frame during world install (blocked on Phase C
  bake-order rework).
- Intra-sim parallelism with deterministic reduction, if a single sim ever needs it.
- PathSet→PathSet transform blocks (relaxers, bundlers, pruners).
