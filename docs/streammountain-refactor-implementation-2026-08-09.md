# StreamMountain refactor — implementation plan

Executable companion to `docs/streammountain-refactor-plan-2026-08-09.md` (the
analysis). That document holds the measurements and the *why*; this one holds
the ordered steps, the exact anchors, the gates, and the stop conditions.

**Baseline:** `main` at `48f3b2bb`. Every anchor below was re-verified against
the working tree on 2026-08-09; where the analysis document disagrees, THIS
document is right (§0.4 lists the corrections).

**Prime directive:** every step has a gate. If a gate fails, STOP and report —
do not proceed to the next step, and do not weaken the gate to make it pass. A
step that lands with a failing gate is worse than a step not attempted, because
the next step's gate then has no trustworthy baseline.

---

## 0. Before touching anything

### 0.1 Work in a dedicated worktree

The user continues working on `main`. Do not build or edit there.

```bash
cd "D:/Shared With Desktop/AI/matter-engine-cpp"
git worktree add .worktrees/sm-refactor -b feature/streammountain-refactor main
```

Then bootstrap it — a fresh worktree is NOT buildable as-is:

```powershell
# NTFS junctions: git worktrees on Windows materialise tracked symlinks as text files
$w = "D:\Shared With Desktop\AI\matter-engine-cpp\.worktrees\sm-refactor"
foreach ($p in @("MatterEngine3\shaders","MatterEditor\shaders")) {
  $link = Join-Path $w $p
  if (Test-Path $link) { Remove-Item $link -Recurse -Force -ErrorAction SilentlyContinue }
  New-Item -ItemType Junction -Path $link -Target (Join-Path $w "libs\MatterSurfaceLib\shaders") | Out-Null
}
# Prebuilt third-party archives the worktree lacks (both are needed; missing them
# shows up ONLY at link time, after every .o has compiled successfully)
$main = "D:\Shared With Desktop\AI\matter-engine-cpp"
Copy-Item "$main\third_party\ozz-animation\build" "$w\third_party\ozz-animation\build" -Recurse -Force
Copy-Item "$main\third_party\raylib\src\libraylib.a" "$w\third_party\raylib\src\libraylib.a" -Force
```

Verify: `ls MatterEngine3/shaders` must list 9 files. If it lists 0 or errors,
the junction failed and every later build will silently no-op.

### 0.2 Build commands (exact — MSYS2 clobbers TEMP)

```bash
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
make -C MatterEngine3 -j8 TMP="C:/Users/webde/AppData/Local/Temp" TEMP="C:/Users/webde/AppData/Local/Temp"
make -C MatterEngine3/tests run-scatterprof TMP="C:/Users/webde/AppData/Local/Temp" TEMP="C:/Users/webde/AppData/Local/Temp" GRAPHICS=GRAPHICS_API_OPENGL_43
```

Check builds by **exit code**, never by grepping for `error:` — a missing
junction produces `No rule to make target 'shaders/raster.vs'`, which matches no
error grep. Redirect long builds to a log file under the scratchpad, not `/tmp`.

### 0.3 Git hygiene — three traps, all of which have bitten this repo

1. **Never `git add -A` / `commit -a` in a worktree.** The shader junctions read
   as deleted symlinks and get recorded as deletions. This happened on
   `ebd226d6` and was caught only because the merge refused. Stage explicit
   paths. Before every commit, verify:
   ```bash
   git ls-tree HEAD -- MatterEngine3/shaders MatterEditor/shaders
   ```
   Both must be present as mode `120000`. `git status` does NOT report this and
   `git show --stat` lists the path either way — `ls-tree` is the only reliable
   check.
2. **Never `git stash`** in a worktree — it destroys the junctions.
3. **`main` has unrelated uncommitted edits** (4 deleted `StressForest*.js`,
   blank-line reformat of `alpine_ecology.js`). They are not yours; the worktree
   starts from committed `main` and will not see them. Do not "restore" them.

### 0.4 Corrections to the analysis document

Read the analysis for context, but these five claims in it are wrong or
imprecise. Trust this list:

| Analysis says | Actually |
|---|---|
| `hasHabitat()` lives in `alpine_ecology.js` | It is `WorldSector.js:247` (`const hasHabitat = this.hasHabitat();`), passed in at `:356` |
| JS fallback selected by `habitatAt === undefined` at `alpine_ecology.js:633-646` | Selected by `typeof habitatAt === 'function'` at **`alpine_ecology.js:612`**; the `sampleHabitat` call is at **`:642`** |
| WP0 (move the six Alpine\* modules back) is outstanding | **Already done** in `48f3b2bb`, with a regression guard (`test_shared_lib_only_names_shared_objects`). Skip it. |
| line numbers generally | `alpine_ecology.js` line numbers shift — **anchor by symbol name and re-grep before every edit**, never by memorised line number |
| `kMaxHabitatChannels` location | `terrain_field.h:209`, value 16. `channels_at` is `terrain_field.h:372` and takes `float*` — this is the source of WP3's float32 drift |

Verified-correct anchors you can rely on:

- `WorldSector.js` (StreamMountain copy): noise trio `hash2`/`vnoise`/`patch` at
  `68`/`74`/`83`; `anyBiomeWantsTrees` at `132` (called at `118`); `build()` at
  `165`; `GROVE`/`TUFT` seeds at `229`/`231`; alpine branch `if (isAlpineProfile(table))`
  at `344` with its `return;` at `363`; **everything after `363` is the legacy
  path**; grove loop uses `patch(..., GROVE, ...)` at `371`; grass loop uses
  `patch(..., TUFT, ...)` at `404`.
- `alpine_ecology.js`: `hash2` `108`, `identityChannel` `139`,
  `environmentalDryness` `151`, `sampleHabitat` `296`, `formSuitabilities` `384`,
  `selectAlpineAsset` `470`, `plannedCandidate` `583`, `planAlpineSector` `738`;
  the "spatial grid was a wash, do not retry" note at `707`.
- `dsl_bindings.cpp`: `j_habitatAt` `1197` (channel copy `1212`),
  `j_candidatesInRect` `1309`; both registered at `1649`/`1651`.

---

## 1. STEP 1 — build the verification harness (do this first, always)

Nothing below can be honestly verified without a placement hash. Build it before
changing a single line of scene code.

**What:** in `MatterEngine3/tests/sector_scatter_profile.cpp`, after the
plan/build call for each band, fold every placement into an FNV-1a64 hash and
print it as `placement-hash band=<n> <hex> count=<n>`.

- Fold, per placement, in emission order: module name, canonical params JSON,
  and the position/rotation/scale **as raw bit patterns** (`memcpy` the double
  to `uint64_t`), never as formatted text — `%g` rounding would hide exactly the
  drift you are trying to detect.
- Print the count alongside the hash. A hash that changes tells you *something*
  moved; a count that changes tells you *what kind* of thing moved, and that
  distinction is what makes a failed gate diagnosable.

**Gate (mandatory):** run `run-scatterprof` **twice** without changing anything.
The hashes must be identical across runs, for every band. If they are not, the
harness is sampling nondeterminism (map iteration order, a time seed, thread
interleaving) and is unusable as a gate — **STOP and report**. Do not proceed
with a nondeterministic baseline.

**Record:** commit the harness on its own, then save the baseline output to
`docs/perf/streammountain-baseline-<date>.txt` and commit that too. Every later
step diffs against this file.

**Commit:** `test: placement-hash gate for the StreamMountain scatter harness`
(stage `MatterEngine3/tests/sector_scatter_profile.cpp` and the baseline file
explicitly).

---

## 2. STEP 2 — delete the dead halves (analysis WP1)

Expected: **≈ −370 lines, zero drift.** This is the highest value-to-risk step
in the plan: it is pure deletion, and the gate is bitwise.

### 2.1 Establish that the code really is dead

Do not trust the analysis. Prove it, then delete:

```bash
# StreamMountain sets the alpine profile => isAlpineProfile(table) is true
grep -n "profile" projects/world_demo/scenes/StreamMountain/StreamMountain.js
```

The alpine branch at `WorldSector.js:344` returns at `:363`. Confirm
`__vegetation.profile` is `'alpine-lush'` for this scene, and that
`isAlpineProfile` returns true for it. **If that is not unconditionally true for
every rung and every biome this scene bakes, STOP** — the "dead" code is live
and the whole step is void.

### 2.2 Delete, in this order (one commit per bullet is fine)

**a. `scenes/StreamMountain/objects/WorldSector.js`** — everything after the
`return;` at `:363` that belongs to the legacy path: the tree-grove loop, the
grass-clump loop, the `GROVE`/`TUFT` seed constants (`:229`/`:231`),
`anyBiomeWantsTrees` (`:132`) and its call site (`:118`), the legacy
`Grass`/`Tree` entries in `assetVariants` (built then discarded by
`selectVegetationCatalog`), and the pebble remnants.

- KEEP `scatterRocks`, the boulder path, the alpine planner call, the cell loop.
- KEEP `hash2`/`vnoise`/`patch` (`:68-90`) — after this deletion their only
  consumer is the `SCREE` channel in `scatterRocks`. Add a one-line comment
  saying so, so the next reader does not delete them as orphans.
- Delete the historical comments that narrate the code you just removed. A
  comment describing a deleted loop is worse than no comment.

**b. `shared-lib/alpine_ecology.js`** — `sampleHabitat` (`:296`), its private
noise stack (`hash2` at `:108` and the vnoise/fbm helpers that only it uses —
check each for other callers before deleting), `environmentalDryness` (`:151`),
and the fallback arm of the `typeof habitatAt === 'function'` branch at `:612`
(the `sampleHabitat` call at `:642`).

Replace the branch with an assertion, not a silent path: under the alpine
profile a missing habitat tape must fail loudly with a message naming the world
hook that is missing. A quiet fallback is what let this code stay unreachable
and unnoticed for so long.

- **KEEP `candidatesInRectJs`.** It is the executable spec the native grid is
  diffed against — deleting it removes the only proof the native port is
  faithful. This is the opposite case from `sampleHabitat`, which could never be
  diffed against the tape (different constants by design, per the `[0,1]→[−1,1]`
  note at `:199-214`).

**c. `scenes/StreamMountain/StreamMountain.js`** — shrink `biomes()` to the keys
actually read (`__terrain`, `__vegetation`, per-biome `rocks`). Verify by
grepping the surviving `WorldSector.js` for each key you intend to drop.

**d. `projects/world_demo/tests/alpine_ecology_tests.mjs`** — drop the
`sampleHabitat` / `environmentalDryness` suites; keep catalog and selection.

### 2.3 Gates

1. `run-scatterprof` placement hashes **bitwise identical** to the Step 1
   baseline, every band. Any change means something deleted was live — revert
   and re-examine 2.1.
2. Suites green: `run-world-definition`, `run-evalworld`, `run-sectorbake`,
   `run-script`.
3. **Non-alpine regression check:** the six sibling scenes share
   `alpine_ecology.js`. Load one non-alpine streaming scene cold and confirm it
   still fills:
   ```bash
   rm -rf projects/world_demo/.cache/StreamMeadow
   cd MatterEditor && MATTER_WORLD=StreamMeadow MATTER_SCREENSHOT=<scratch>/sm.png \
     MATTER_SCREENSHOT_SETTLE=120 MATTER_HIDE_UI=1 ./build/windows/editor.exe
   ```
   Require `bake finished (0 errors)`, zero `cannot read child`, and **21 child
   variants** (StreamMountain is 92 — a collapsed count means modules stopped
   resolving).

---

## 3. STEP 3 — batch the habitat boundary (analysis WP2, the headline)

Expected: **−20–25% band-5 wall, zero drift, ≈ +90 engine / −40 scene lines.**

The measurement that justifies this: `plan.habitat` is 7,899 calls/bake at
19.2 µs each, of which the native tape math is ~3–4 µs. **The cost is the
crossing, not the math.** So the fix is fewer crossings, not faster math.

### 3.1 The new binding

Add `__planCandidates(seed, kind, minDist, x0, z0, w, h)` to
`dsl_bindings.cpp`, registered next to the existing pair at `:1649-1651`.

Internally it is the two existing halves welded together, and you should reuse
them rather than rewrite:
- candidate generation: the cell loop already in `j_candidatesInRect` (`:1309`)
- per-survivor channels: `SurfaceRuntime::channels_at` (`terrain_field.h:372`),
  called exactly as `j_habitatAt` does at `:1212`

Return **flat typed arrays**, not an array of objects — the per-candidate object
allocation is part of what you are removing. One `Float64Array` of stride
`5 + channelCount` is the simplest shape; document the stride layout in a
comment above the binding, because the JS side indexes it by hand.

When no habitat tape is bound, return the candidate fields with
`channelCount = 0` rather than failing — the binding must be usable by scenes
that have no tape.

### 3.2 The JS side

In `alpine_ecology.js`, `plannedCandidate` (`:583`) currently writes into
`HABITAT_OUT`/`HABITAT_SCRATCH` and copies 12 channels per candidate. That
plumbing and the per-candidate `habitatAt(...)` call both disappear; selection
reads channels straight out of the flat array by `HABITAT.*` index.

**Leave the boulder loop on the old `candidatesInRect` binding.** It runs at
0.13 candidates/cell and is measured at 0.0% of the bake — unifying it buys
nothing and risks a second consumer of a changing shape.

**Keep `__habitatAt` and `__candidatesInRect` working.** Contexts without the
new binding still need them, and the JS-spec cross-check test extends to the
batched form (same candidates, same channels).

### 3.3 Engine-purity check (state this in the commit message)

The engine learns: *"evaluate the bound tape at each surviving grid candidate
and return channels by index."* Channel **names** never cross into C++ —
`channel_regs` is already index-keyed. No family, no species, no ecology. This
is the same abstraction level as `channels_at` itself, and the same move
`candidatesInRect` already made one level down.

### 3.4 Gates

1. **Placement hashes bitwise identical.** Same doubles, same order, same
   `channels_at` path — the `candidatesInRect` precedent proved bitwise identity
   is achievable for exactly this shape, so accept nothing less. If they differ,
   the most likely cause is candidate *ordering* changing between the fused loop
   and the two-pass original — check that before touching anything else.
2. Re-run `run-scatterprof` and record the new per-band wall times against the
   baseline. **Report the real number even if it is below the predicted
   −20–25%.** A measured −8% honestly reported is a useful result; a predicted
   −22% restated as fact is not.
3. Suites: `run-sectorbake`, `run-evalworld`, `run-script`, `run-terrainfield`.

---

## 4. STEP 4 — suitability into the tape (analysis WP3) — **MEASURE FIRST**

This step is **gated on a measurement and may be correctly skipped.** Do not
start it because it is next in the list.

### 4.1 The gate

Split `plan.select` with two new profiler slots — `plan.suitability` (the
`formSuitabilities` products, `:384`) vs `plan.identity` (the `identityChannel`
`Math.sin` hashes, `:139`) — using the existing `profSlot`/`profBegin`/`profEnd`
DSL API. Re-run `run-scatterprof`.

- If **suitability dominates** → proceed with 4.2.
- If **`identityChannel` dominates** → skip 4.2 entirely and report that the
  better move is an integer-hash swap (bigger win, one-time jitter reshuffle of
  species/scale/rotation with positions unchanged). Do not do the swap without
  checking in — it changes placements.

The profiler exists precisely so this fork is settled by data. Hot-path guesses
in this codebase have been wrong twice.

### 4.2 If the gate says proceed

Emit each form's suitability as an extra anonymous tape channel (12 + 19 = 31),
raising `kMaxHabitatChannels` from 16 to 32 at `terrain_field.h:209`. That
constant is a stack-budget bound with no GPU mirror — confirm that is still true
by reading its surrounding comment before changing it, and check the register
file's stack footprint at 32.

JS keeps identity jitter, argmax, mixture sampling, acceptance.

**Drift is real here and must be reported as a number, not a word.**
`channels_at` is `float*`: float32 tape versus float64 JS flips the argmax at
near-ties, so a small fraction of placements change species/form (never
position). Expect <1–2%. Report the measured percentage from the placement-diff
tooling. **Do not claim "identical".**

---

## 5. STEP 5 — readability pass (analysis WP4)

Only after the deletions land. Expected −40 to −70 lines, no drift.

- `build()` reads as four named phases: terrain / boulders / rocks / alpine
  plan+place.
- Collapse `put`/`putPlanned` into one helper (after Step 2 the only legacy
  `put` consumer is rocks).
- The `hash2`/`vnoise`/`patch` trio keeps a one-line note on why `SCREE` does
  not use the tape (placement stability across tape-hash changes).

**Do NOT** do the optional "replace SCREE with a tape channel" variant. It
re-patterns rock fields — medium drift — and needs a human decision. Mention it
in your report instead.

---

## 6. Out of scope — report, do not do

- **Deleting the `WorldSector.js` template or consolidating the seven copies**
  (analysis WP5). The per-scene duplication is a deliberate decision the user
  made this session. It is theirs to revisit.
- **The native family planner** (analysis WP6). Contingent on Steps 3–4 landing
  and being re-measured, and it needs the integer-hash swap first. Do not start.
- **`scenes/StreamMountain/props.json`.** It currently sets `hide: true` on all
  six vegetation modules and `Rock`, plus a `stream.lod` override. It is the
  user's saved tuning — **do not edit or delete it.** If you need a visual
  replay, copy it aside, neutralise the copy, run, and restore; say in your
  report that you did. Note that any replay run without doing so "verifies" an
  empty mountain.
- **Retrying spatial-grid tree exclusion.** Already built, proven bitwise
  identical, measured a wash, reverted. The note is at `alpine_ecology.js:707`.
- **Touching `surfaces()`, `sector.terrain`, or porting channel names/catalog
  into C++.**
- **The ~54 ms/bake fixed overhead** on vegetation-free tiles (fresh QuickJS +
  evaluating the 786-line ecology for tiles that never plant). Plausibly a
  larger cold-fill lever than everything above, and untouched by this plan.
  Worth flagging as the next investigation; do not start it here.

---

## 7. Reporting

Report after **every** step, not just at the end. Each report states:

1. What changed (files, line delta).
2. The placement-hash result: identical, or the measured % of placements changed.
3. The measured timing delta per band, against the Step 1 baseline file.
4. Which gates passed, and **the exact command and output** for each — not a
   claim that they passed.
5. Anything you could not verify, said plainly.

If a gate fails: stop, report the failure with its output, and state what you
think it means. Do not carry a red gate forward, and do not adjust a gate to
make it green. Estimates must never be presented as measurements — if you could
not run something, say so.
