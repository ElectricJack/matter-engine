# Representation: a ground-up redesign of LOD + texturing

Date: 2026-08-04
Status: DESIGN — nothing in this document is implemented.
Inputs: `docs/lod-vt-system-walkthrough-2026-08-04.md` (the audit of what exists) and Jack's
requirements feedback of 2026-08-04. Companion: `docs/superpowers/plans/2026-08-04-lod-vt-migration.md`
(how to get there from here).

This is the system we would build if we were starting over, knowing everything the current
system taught us. It is deliberately smaller than what exists.

---

## 1. Requirements

Distilled from the feedback, numbered so the rest of the document can cite them:

- **R1 — One world path.** No closed/streaming split. A single pipeline serves Demo and
  StreamMountain alike.
- **R2 — The DSL owns LOD generation.** A part fully defined by script should define how its
  own LODs are generated, including **staged generation**: expensive intermediate data (a
  tree's branch hierarchy) computed once and available to any later LOD build. The engine
  provides defaults and library generators, not a mandatory automatic ladder.
- **R3 — Generate and persist only what is read.** The twelve-artifact zoo shrinks to the set
  something actually consumes.
- **R4 — Centrally orchestrated, discrete, deterministic switching.** Mesh, VT, BLAS/TLAS and
  instances for one object switch **together**, at discrete distances, atomically. Terrain and
  objects may pop at different times (preferred, even), but each switch is whole. Flying the
  same path forward and backward produces the same pops.
- **R5 — Distances, not error.** The person tuning a part or world specifies switch distances.
  Error estimation exists only to propose **default** distances the author can accept or edit.
- **R6 — The engine exposes only global budgets.** Main-thread time per frame, and memory
  (system RAM, disk, VRAM). Everything else adapts to fit inside those.
- **R7 — Smooth frame times, minimal popping and re-work.** Returning to an area already baked
  is near-instant compared to the initial bake.
- **R8 — A real asset storage foundation.** Binary data, loaded and saved in large chunks with
  fast async file I/O. Designed as a standalone MatterEngine subsystem first, then used as the
  foundation of all caching.
- **R9 — Visibility-gated streaming.** The engine should not load or compute what the camera
  provably cannot see: travelling down a canyon, nothing beyond its walls is baked, read, or
  resident. A very coarse whole-world pass computes sector visibility, visible sectors
  generate nearest-first, and that same coarse world doubles as the always-present backdrop
  that grounds high-resolution detail as it pops in.

Non-goals: changing the `.gtex` material bake itself, changing the RT lighting core,
networked/multi-client streaming, animated impostors.

---

## 2. The core concept: Representation

The audit's thesis was that four systems each depend on "the drawn LOD rung" and none owns the
concept. The redesign starts by making that concept a first-class noun and giving it exactly
one owner.

> A **Representation** (rep) is one complete drawable form of a part: geometry, its UVs into
> the part's shared parameterisation, its BLAS source, and its material bindings. A mesh rung
> is a rep. A traced card set is a rep. An impostor is a rep — **the last one**, not a
> parallel system.

Every part has an ordered **RepSet**: rep 0 (the authored `build()` output) through rep N
(typically an impostor), each with an authored **switch distance**. "LOD level" disappears as
a vocabulary item; there are only rep indexes into one table.

The single rule that replaces seven selectors, two impostor pipelines, and the per-frame
partition machinery:

> **Every consumer of "what do I draw for this instance" reads one committed rep id.
> Nothing else derives it.**

Mesh draw, VT mip demand, BLAS selection, skin planning, impostor draw — all read the same id.
There is one function that produces it (§5), evaluated in exactly two places, and one clamp
(residency) that can delay it but never redefine it.

---

## 3. Authoring: the DSL owns the recipe (R2, R5)

### 3.1 The `lods()` method

A part class may declare its representations. In the engine's existing idiom:

```js
class AlpineConifer extends Part {
  static params = { seed: 0, dryness: 0.35, size: 1.0, form: 0 };

  // A STAGE (§3.2): computed at most once, cached in the store, available to
  // any rep builder that asks. This is where the tree derives its branch
  // hierarchy for every coarser rep to reuse.
  skeleton(p, ctx) { /* ... returns branch graph ... */ }

  build(p, ctx) { /* full-detail geometry, exactly as today */ }

  lods(p) {
    return [
      { at: 0 },                                       // rep 0: build() verbatim
      { at: 18,  gen: LOD.remesh({ voxel: 0.06 }) },   // engine library generator
      { at: 45,  gen: (ctx) => this.crownCards(p, ctx.stage('skeleton')) },
      LOD.impostor({ at: 140, views: 24 }),            // explicit terminal (§3.4)
    ];
  }
}
```

Semantics:

- `at` is the switch-in distance in metres for a unit-scale instance. An instance's effective
  distance table is `at × instance_scale × lod_bias`. Nothing else enters the selection
  function — no pixel budget, no error term, no feedback (R4's determinism follows from this).
- `gen` is either an **engine library generator** (`LOD.remesh`, `LOD.simplify`, composable:
  `LOD.remesh({voxel}).then(LOD.simplify({error}))`) or a **script builder** — an ordinary
  function using the same drawing API as `build()`, with access to stages. Custom builders
  are how a part exploits its own structure (crown cards from a skeleton) far more
  efficiently than any generic remesher can.

### 3.2 Stages: order, execution, and persistence

**Order is data flow, not declaration.** There is no stage list and no numbering. A stage is
an ordinary named method; anything running under the bake context — `build()`, a rep builder,
or another stage — pulls one with `ctx.stage('name')`. That call ensures the named stage is
up to date (running it first if it has never run) and returns its output. Execution order is
therefore the topological order of the demand graph and nothing else; a dependency cycle is
a bake error naming the cycle. The position of entries in `lods()` never influences stage
order — rep 3 demanding `settle` before rep 1 is ever baked is perfectly normal.

**When stages run.** On the bake workers, the first time something demands them — never
eagerly. Baking rep 3 cold runs exactly rep 3's transitive stage closure and nothing more.
A stage no rep or build ever demands never runs at all.

**How stage data persists.** Every stage output is serialized to a blob in the store (§9)
under `(part hash, stage name, version vector)`, alongside a recorded **dependency trace**:
the list of stages it consumed and the content hash of each consumed output. A later bake —
minutes or days later, warm or on another machine — validates the trace bottom-up and
reloads the blob instead of re-running the stage. The memo key folds the stage's **own
method source**, the params, and its inputs' *output* hashes, which buys early cutoff: if
you edit `settle` but it reproduces bit-identical resting poses, everything downstream
still hits. Editing one rep's builder invalidates only that rep; editing a shared module
invalidates every part that imports it (module-closure hashing — coarse but honest).

Stage outputs must be plain serializable data (positions, transforms, graphs) or engine
blob handles (a mesh). A closure or function in a stage result is rejected at bake with a
clear error. Stages must be deterministic — seeded RNG, fixed timesteps — which is the same
discipline the tileset settle cache already proves out today; the stage system is that
pattern generalized and made available to scripts.

**Worked example — physics settle → placement → per-rep foliage:**

```js
class ForestKnoll extends Part {
  static params = { seed: 7, boulders: 12 };

  // Stage: scatter boulders and physics-settle them over a fixed number of
  // timesteps. Deterministic: seeded scatter, fixed dt, fixed step count.
  settle(p, ctx) {
    const bodies = scatterBoulders(p.seed, p.boulders);
    return ctx.physics.settle(bodies, { dt: 1 / 60, steps: 240 });
  }

  // Stage: foliage PLACEMENT derived from where the boulders came to rest —
  // ferns in the lee, moss on shaded faces. Placement is decided once, HERE,
  // not per rep, so every rep agrees on where each plant lives.
  foliagePlan(p, ctx) {
    return planFoliage(ctx.stage('settle'), p.seed);
  }

  build(p, ctx) {
    const rest = ctx.stage('settle');
    const plan = ctx.stage('foliagePlan');
    /* full boulders + full-geometry plants at the planned positions */
  }

  lods(p) {
    return [
      { at: 0 },
      // Same plan, cheaper realization: card plants at the same positions.
      { at: 35,  gen: (ctx) => this.cardFoliage(p, ctx.stage('foliagePlan')) },
      // Same plan again: nearby entries merged into clumps, boulders remeshed.
      { at: 110, gen: (ctx) => this.clumps(p, ctx.stage('foliagePlan'), ctx) },
      LOD.impostor({ at: 300, views: 24 }),
    ];
  }
}
```

The settle runs **once, ever** — the first time any rep of this part is baked. Rep 2, baked
weeks later when the camera first sees this part at range, reloads the settled poses and the
plan from the store and runs only `clumps`. And because *placement* is a stage while
*realization* is per-rep, a rep switch changes what each plant looks like but never where it
stands — the geometry analogue of §7's shared parameterisation, and part of why pops read as
calm rather than as reshuffles.

### 3.3 Where the ladder stops: a benefit rule, not an error rule

Observed on the shipping build 2026-08-04, with the LOD-tint and wireframe views on: a rock
keeps changing rung — the tint cycles — while its silhouette stops changing. The cause is
that today's ladder is driven by **error tolerance and never by benefit**.

`part_flatten.h:27` gives every part the same nine rungs regardless of what it is:

```
radius_divisor = {512, 256, 128, 64, 32, 16, 8, 4, 2}
eps_i = bound_radius / divisor_i
```

and the only condition for a rung to exist is `geo.size() < prev_count` — *strictly fewer
triangles than the rung before*. One triangle qualifies. So the coarse tail runs
26 → 24 → 22 triangles: three rungs, three colours, one silhouette. Measured on
RockGallery, parts bottom out between 18 and 96 triangles, and the last several rungs each
shave single digits. Nothing in the pipeline ever asks whether a rung bought anything.

**The rule the redesign adopts instead:** keep adding mesh reps while decimation still buys
a real reduction; the moment it does not, stop — **and that index is where the impostor
goes**. Concretely, a rep is admitted only if it clears a *benefit* floor (a substantial
triangle reduction, or a measured silhouette change) and not merely a non-zero one.

Two things fall out of this that a tuned rung count cannot give:

- **It is self-terminating per part.** A 4,258-triangle boulder and a 58-triangle pebble
  earn different ladder depths automatically. Today they both get the fixed nine-rung
  treatment, and the pebble spends four rungs achieving nothing.
- **It answers "at which rung does the impostor go?"** — a question §3.4's `LOD.impostor`
  terminal otherwise leaves to the author's guess. The bottom-out point is the principled
  answer, and it is the point past which more mesh rungs are provably wasted work.

Note this is orthogonal to close-range fidelity. **Rep 0 is the raw mesh, undecimated** —
the ladder never removes anything at close range, so "more detail up close" is a question
about the part's own build resolution, not about LOD. That too is a `lods()`/build-recipe
concern (§3.1): a part should be able to declare the resolution it is built at, which today
is fixed engine-wide.

### 3.4 The impostor and vanish callouts

The switch to an impostor is the most consequential transition in the table, so it is
syntactically explicit rather than buried in a `gen`:

- `LOD.impostor({ at, views, ... })` is a distinguished terminal entry. Validation enforces
  **at most one, and only in last position** — a mesh rep after an impostor is a bake error.
  Reading a part's `lods()` tells you at a glance whether it ever impostors and at exactly
  what distance.
- For an assembly, the impostor **replaces the entire subtree** (`replaces: 'subtree'` — the
  default and the only v1 mode, matching today's feature; written out so the reader sees the
  contract at the callsite).
- `LOD.vanish({ at })` is the other explicit terminal: past `at`, the instance is culled
  entirely. This is the honest recipe for litter and small ground detail, which today either
  over-lives or falls out implicitly.
- Omitting both means the last mesh rep holds at any distance — never a silent impostor.

At runtime none of this is special: the impostor (or vanish) is still just rep N of one
table, selected by the same walk (§5). The explicitness is purely an authoring-surface
guarantee.

### 3.5 Lazy, per-rep baking

`lods()` is a *declaration*, not a work order. Nothing is generated until the streamer
requests a specific `(part, rep)` pair — a sector entering at far range bakes **only** the far
rep and whatever stages it needs. This single property deletes, by construction:

- the eager full-ladder bake on every static part,
- the terrain-ladder waste (terrain sectors get a terrain recipe; nothing bakes nine QEM
  passes to be discarded at `part_store.cpp:1166`),
- most of the initial-bake latency the current system pays before anything appears.

### 3.6 Defaults (R5's second half)

A part with no `lods()` gets the **default recipe**: the current adaptive ladder, repackaged
as a library generator with two fixes from the audit — a *measured* geometric error per rep
(actual mesh-to-mesh deviation, not the synthetic `prior + 0.9 × remaining` schedule), and
distances derived from that error via the projected-size relation. The projected-size metric
survives *only* here, at bake time, as a default-distance estimator; it is gone from runtime.
The default recipe ends in an explicit `LOD.impostor` entry for parts that meet today's
eligibility policy — so the editor's effective-table view (below) always shows the impostor
distance, defaulted or authored, in the same place.

Tooling requirement: the editor shows every part's **effective** distance table (authored or
default) and can copy a default table into script text with one action, so tuning is
"promote the default, then edit numbers" — never "reverse-engineer the estimator."

Skinned parts drop their hardcoded `BakeTargets` and get a default recipe like everything else.

---

## 4. One world path (R1)

**Every world is a streamed world.** The sector streamer — the one subsystem the audit rated
"genuinely good" — becomes the *only* way geometry enters the scene.

- A "closed" world is a **policy**, not a pipeline: a finite sector grid covering the authored
  bounds, `residency: all`, eviction disabled. Demo boots by streaming all of its sectors in,
  exactly the way StreamMountain streams a ring in.
- `compose_world` and its up-front manifest are deleted. The three separate QuickJS
  evaluations collapse to one: the world module is compiled **once** into a `WorldProgram`
  (explicit `export default` / registration replaces the `find_world_class_name` regex);
  statics enumeration, `field()`/`surfaces()`/`biomes()`, and per-sector evaluation all
  instantiate from that one compiled program (per-sector worker isolates spawn from its
  bytecode, not from a re-parse).
- **"Sector" gets one meaning**: a streamer cell. The pitch-16 spatial grid is renamed
  (`LocalityGrid`) and becomes an internal index never called a sector; the JS module keeps
  its own name.

What this buys beyond conceptual hygiene: every fix to publish slicing, budgets, residency
and commit atomicity (§5, §8) automatically applies to small worlds too, instead of the
closed path being a second implementation that drifts.

---

## 5. Selection and orchestration (R4)

### 5.1 One pure function

```
desired_rep(instance) = largest k such that table[k].at × instance_scale × lod_bias ≤ distance(eye, instance_center)
```

Evaluated in exactly two places:

1. **CPU, coarse** — the streamer/planner evaluates it per instance-group to decide what to
   *prefetch and bake* (this is what makes rep k resident before the camera crosses its
   distance).
2. **GPU, exact** — the cull shader evaluates it per instance to decide what to *draw*,
   clamped to the committed residency window `[min_resident, max_resident]` for the cluster.
   The clamp can hold you on the previous rep while the next one loads; it can never choose
   a different rep than the function would.

VT mip demand, RT BLAS choice, and skin planning all read the committed rep id (or the same
function where they need look-ahead). `SectorLodResolver`, the terminal-impostor CPU
comparison, the CPU mesh-rung mirror as an independent selector — all deleted. Seven
selectors become one function, two evaluators, one clamp.

### 5.2 Atomic switches

A rep switch for a cluster **commits** only when the target rep's full resource set is
resident: geometry uploaded, BLAS built, and its texture mip level composited (§7). The
commit itself is one small buffer write, batched under the main-thread budget (§8). The
outgoing rep's resources are evicted only after commit + a grace period.

Consequences, matching R4 exactly:

- Mesh, texture and RT presence for an object change **in the same frame**, by construction.
- Under I/O or bake pressure a switch is *delayed*, never *partial* — you keep seeing the old
  rep, whole, until the new one is entirely ready.
- Hysteresis is a distance band (switch out at `d`, back in at `~0.92·d`), so each direction
  of travel has its own deterministic switch point and the boundary never flickers.
- Terrain keeps its own schedule (ring-boundary snaps) and objects theirs — different pop
  *times*, per the stated preference, but every individual pop is whole.

### 5.3 Subtree replacement without order-coupling

Today, "an impostor suppresses its subtree" is encoded as *record order in the instance
buffer* — load-bearing across four files that never reference each other. In the redesign,
expansion is **rep-indexed at stage time**: rep k of an assembly maps to an explicit instance
list (rep N is usually a single impostor quad). The GPU picks a rep; the indirect draws come
from per-rep partitions built once when the sector stages, not from per-frame CPU record
walks. The two per-frame state machines, the full array copy and the unconditional ~14 MB
re-upload all delete; suppressed instances genuinely leave the dispatch.

---

## 6. The proxy world: occlusion-gated, nearest-first streaming (R9)

The camera in a canyon should cost what the canyon costs — nothing beyond its walls should
be baked, read, or resident. The mechanism is one new tier with two jobs:

> The **proxy** is the world's coarsest representation: per-sector, always-resident,
> whole-world, baked one to two orders of magnitude coarser than the coarsest streaming
> rung. Job one: it is the **occluder** from which sector visibility is computed. Job two:
> it is the **backdrop** that draws wherever real reps aren't committed yet, grounding
> high-resolution detail as it arrives.

### 6.1 What it is and where it comes from

- The proxy is produced by the same rep machinery as everything else — it *is* a rep, so
  identity, versioning, caching and invalidation come for free. For terrain it is a very
  coarse field march; the occluder set is terrain-only in v1 (§12, Q5).
- Baking it is the **first pass over any world**: cheap per sector, embarrassingly
  parallel, and written contiguously in the store (§9) so a warm session loads the entire
  world's proxy in one or two sequential reads at open.
- It must be small enough to be **unconditionally resident** — target single-digit MB per
  square kilometre: coarse mesh, per-vertex material derived from the tape, no pages, no
  per-part instancing. It is never evicted; the memory budgets (§8) govern everything
  above it.
- It **subsumes two existing mechanisms**: the terrain far-field impostors (StreamMountain's
  25 mode-1 `.fimp`) and the fog-wall ring trim. Beyond the streaming rings you see proxy,
  not fog.

### 6.2 Visibility

Each frame (amortized under camera motion) the proxy renders to a small depth buffer; a
HiZ pyramid is built; every candidate sector's bounds test against it and classify as
**visible**, **offscreen**, or **occluded**.

The rule that makes coarse occlusion safe for *loading* decisions: the occluder variant of
the proxy is **eroded at bake** — offset inward by at least its own voxel error — so it can
never occlude anything the true surface would not. A coarse proxy may under-occlude (waste
a little work); it must never over-occlude (a hole when you round the corner). Erosion is
the conservative direction, and it is directly testable (the migration plan's
erosion-safety gate).

Prediction, not reaction: the visibility test uses a widened frustum swept along
short-horizon camera motion, and hysteresis keeps recently visible sectors warm — a camera
turn should not stare at raw proxy while a burst of bakes catches up.

### 6.3 What visibility drives

**Priority — never selection.** Consistent with §8's contract (budgets shape *when*, never
*what*), the desired rep remains the pure distance function; visibility reorders the work
queue:

1. **Visible** sectors stream nearest-first — the entire bake/IO budget goes down the
   canyon you are actually in.
2. **Offscreen** near sectors follow (turn insurance).
3. **Occluded** sectors come last — and beyond a distance threshold, not at all: no bake,
   no read, no residency. This is "do not load or compute anything outside the canyon,"
   literally.

Determinism survives because visibility is a pure function of camera pose against a static
proxy: the same fly-through yields the same classification sequence, hence the same commit
order. The keystone determinism test gains a canyon path.

### 6.4 Grounding the pop

The proxy is the floor of the visual ladder. Wherever a sector has no committed rep, proxy
draws; when the sector's first real rep commits, its proxy hides — and that swap is itself
an atomic commit (§5.2): never both, never a gap. Because the proxy is shaded from the same
tape-derived material the real world uses (§7), arrival reads as *sharpening*, not
*materializing*. Cresting a canyon rim therefore looks like: a complete soft world
instantly (the proxy was always resident), then detail pouring in nearest-first.

Optional and v1-deferred: the proxy's BLAS can stand in for far geometry in RT, giving
distant shadows and GI something to hit beyond the streamed rings.

---

## 7. Texturing: one parameterisation, a real mip chain

The root cost driver in the audit was per-rung charting: six call sites, each rung a complete
independent texture world. The fix is structural:

- **One parameterisation per part.** Charting runs once, on rep 0. Every other rep's bake
  reprojects onto that same parameterisation and stores UVs into the shared atlas. (This
  happens at *bake* time, where the current system pays for a nearest-triangle reprojection
  per rung at *runtime* for the warp frame.)
- **The page pool becomes a true mip chain.** Distance selects a mip of one persistent atlas
  rather than a different atlas per rung. A rep switch changes geometry only; texture content
  persists across it and sharpness transitions are trilinear — popless. This is the direct
  answer to "the terrain mesh pops, then textures bake in incrementally with very low
  persistence."
- Pages composite per `(part, mip)`, not per `(part, rep)`: resident texture state per sector
  drops by roughly the rung count.
- **`.gtex` is unchanged as the material source.** Its horizon channels are henceforth sampled
  in the **tile's own frame**, via the shared parameterisation's rung-invariant tangent basis —
  which closes the dark-dome-patch defect *by construction*, since the query can no longer
  depend on any per-rep basis.
- **Occlusion gets one owner and exactly two terms**: baked AO (the `.gtex` AO and tier-2
  hemisphere enrichment composited once into the page's ORM.r) and the runtime RT term. The
  live `.gtex` AO ratio and the duplicated horizon-mean application are deleted; one strength
  knob remains.
- **Warp** is solved once per sector against rep-0 geometry and shared by every rep through
  the shared parameterisation; the per-rung reprojection path is deleted.

Open question (deliberately deferred to measurement, §12): whether composited pages are
*persisted* in the store or recomposited on load. They are deterministic functions of
`(rep geometry, chart table, tileset, tape)`, so both are correct; the store supports either
as a policy flag.

---

## 8. Budgets and the frame contract (R6, R7)

The engine exposes **three global constraints and nothing else**:

1. `main_thread_budget_ms` — total per-frame time for all streaming-side main-thread work.
2. Memory budgets — system RAM working set, VRAM pool sizes, disk cache cap.
3. Worker pool size (defaulted from core count).

One **governor** owns the main thread's streaming time: publish slices, rep commits, uploads
and evictions all pass through a single queue with per-frame millisecond accounting. This
carries an interface obligation the current code lacks: **every job must be sliceable**. The
indivisible `stream.publish` is rebuilt as resumable slices (commit / world-state apply /
registration each yieldable), so one slow sector can no longer blow a frame.

The load-shedding contract that preserves determinism:

> **Budgets shape *when*, never *what*.** Selection targets are a pure function of distance;
> pressure delays commits (the residency clamp holds) and evicts farthest-first, but never
> changes which rep an instance is heading toward.

So a slow disk produces late-but-identical pops, not different ones — warm runs over the same
path are bit-identical in switch order (this is testable, and the migration plan tests it).

---

## 9. MatterStore: the asset storage subsystem (R8)

Designed standalone — its own library, tests and benchmark — then adopted as the foundation
of every cache. Nothing in it knows what a "part" is.

### 9.1 Shape

Two layers:

- **BlobStore** — content hash → bytes. Blobs live in append-only **pack files** (64–256 MB).
  An in-memory index (hash → pack, offset, length, checksum) loads from an index file at
  open; index updates are batched and swapped atomically (write tmp, rename). A blob appended
  but not yet indexed is invisible and reclaimed by compaction — crash-safe by construction,
  same discipline as the current `.tmp`-then-rename but amortised over thousands of artifacts.
- **RefTable** — semantic key → blob hash + metadata (kind, size, last-access). Keys are
  `(part hash, kind, rep index, version vector)`. Eviction is LRU over refs against the disk
  budget; compaction rewrites surviving blobs and drops orphans.

### 9.2 I/O model

- **Async reads, batched.** The API takes a batch of refs and returns completions
  (future/callback marshalled to the caller's thread). Windows implementation is
  overlapped/IOCP; large sequential reads optionally unbuffered. Reads land in
  caller-supplied MemoryLib arenas — no per-artifact heap churn.
- **Locality is a write-side concern.** The writer groups one sector's blobs contiguously in
  a pack; compaction reorders by observed co-access. A warm sector revisit is then one or two
  large sequential reads, not today's per-file open/seek storm over thousands of small
  cache files.
- **No image codecs in the hot path.** Geometry is stored raw or LZ4-class; textures are
  stored BC-compressed, GPU-ready. (`.gtex` keeps PNG at its offline layer; what enters the
  store is its decoded, BC-encoded form.)
- Single writer, many readers, cross-process file lock; per-blob checksum, and a bad blob is
  a cache miss, never a crash.

### 9.3 The version vector

The cache layer on top defines **one** version vector — engine bake version, representation
version, box3d version, format versions — folded into **every** RefTable key. This
permanently closes the class of bug found twice this month (the representation version
reaching the resolve key and the impostor hash but not the part hash): there is exactly one
place versions enter keys, so a rule change either invalidates everything it should or the
single fold site is wrong — no more per-artifact-kind plumbing to forget.

### 9.4 Placement

`libs/AssetStoreLib`, depending only on MemoryLib, beside SpatialQueryLib in the dependency
chain. Own unit tests (crash-mid-write recovery, eviction, concurrent read) and a benchmark
harness that measures chunked-pack reads against the current small-file storm — the number
that justifies the subsystem.

---

## 10. Disposition: what dies, what lives (R3)

### 10.1 Artifacts — twelve kinds to four

| Current | Fate |
|---|---|
| `.part` (finest mesh) | → rep 0 blob inside the **PartBundle** |
| `.lods` (serialized ladder) | → per-rep blobs in the PartBundle |
| `.static_lods` + `LMSK` trailer | **delete** — write-only today (only reader is the writer's own cache probe) |
| `.flat.part` | **delete from production** — `world_flatten` is test scaffolding |
| `.fimp` (impostor atlas) | → the last rep's blob in the PartBundle; the terrain far-field mode is retired by the proxy (§6) |
| `.impostor` link files | **delete** — a RefTable row replaces the link indirection entirely |
| `.hints` | fold into the PartBundle manifest |
| chart trailer / chart-atlas serializer | → the shared parameterisation blob (finally gets its production reader) |
| resolve cache entries | → RefTable rows |
| settle cache | → RefTable rows (bake unchanged) |
| `.gtex` | **unchanged** as an offline file; decoded BC form mirrored into the store |
| world tape | **unchanged**, stored as blobs |
| — | **new: world proxy** (§6) — per-sector coarse rep + its eroded occluder variant, always resident, written contiguously |

New persistent set: **PartBundle** (manifest + per-rep geometry + shared parameterisation +
impostor atlas + memoized stage outputs), the **world proxy**, **`.gtex`**, **world
tape/settle**, all inside MatterStore.

### 10.2 Stop generating (independent of any redesign — these are pure waste today)

- The adaptive ladder for terrain sectors (387 MB → 6.6 GB cache inflation, discarded
  unconditionally at load).
- `.static_lods` + `LMSK` (~160 lines of machinery, zero readers).
- The second impostor producer (same object baked twice from two hierarchy walks).
- `SectorLodResolver` output (computed per instance, read by nothing shipping).

### 10.3 Code counts

| | Today | Redesign |
|---|---|---|
| LOD selectors | 7 live (+1 dead) | 1 function, 2 evaluators, 1 clamp |
| Ladder generators | 5, with 4 threshold formulas and 3 error notions | library generators invoked by `lods()`; distances are the only threshold |
| Texture-data paths | 7 (5 live) | 3: page mip chain, `.gtex`, impostor atlas |
| Impostor producers | 2 | 1 (a rep like any other) |
| World composition paths | 2 (closed + streaming) | 1 |
| Script evaluations per world | 3 | 1 compiled program |

Plus the audit §4.5 dead list (`shaders_gpu/cull.comp`, `gpu_cull_types.h`,
`world_composer.*`, `raster_cull.h::cluster_lod_select`, `tileset_provider.*`,
`tileset_macro_slot`, `ResolvedInstance::segment`), which is deleted **before any other work**
per the requirements.

### 10.4 What we deliberately keep

The streamer core (rings, hysteresis, generation tags, publish-then-evict — promoted to the
only world path). The QEM simplifier, marching-cubes surfacing, chart segmentation and
packing, the `.gtex` bake, the warp solver, the BC encoders, the cull shader skeleton, the
double-bake determinism discipline. The projected-size metric — demoted to a bake-time
default-distance estimator.

---

## 11. How the requirements map

| Req | Where satisfied |
|---|---|
| R1 one world path | §4 |
| R2 DSL-owned staged LODs | §3.1–3.4 |
| R3 only what is read | §10.1–10.2 |
| R4 atomic deterministic switches | §2, §5 |
| R5 distances, error as default | §3.1, §3.6 |
| R6 global budgets only | §8 |
| R7 smooth frames, instant revisit | §5.3, §7, §8, §9.2 |
| R8 asset store foundation | §9 |
| R9 visibility-gated streaming | §6 |

---

## 12. Open questions

1. **Persist composited pages, or recomposite on load?** Deterministic either way; a store
   policy flag decided by measurement (disk read of BC pages vs GPU recomposite time).
2. **Card-generator library scope.** Should the engine ship `LOD.cards` helpers (cross-cards,
   shell cards from a skeleton), or is that purely script-side? Proposal: start script-side in
   `shared-lib`, promote to `LOD.*` once two parts want the same helper.
3. **Assembly reps.** Rep-indexed expansion (§5.3) lets an *assembly* declare its own coarse
   reps (a whole hut becoming one mesh, then one impostor). The mechanism supports it; v1
   scopes it to "subtree → impostor" only, matching today's feature.
4. **Existing worlds.** Default recipes keep every current script working unmodified; explicit
   `lods()` is adopted part by part. No migration of project scripts is ever forced.
5. **The occluder set (Q5).** v1 computes visibility from eroded terrain only. Large authored
   structures (a city wall, a fortress) are legitimate occluders too; admitting them needs a
   per-part `occluder` flag and erosion of authored meshes — deferred until a world needs it,
   because a missing occluder only costs work, never correctness.
