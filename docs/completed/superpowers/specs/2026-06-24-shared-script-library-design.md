# SP-7 — Shared Script Library — Design

**Status:** Approved for planning (2026-06-24)
**Project:** MatterEngine3
**Parent:** `2026-06-24-procedural-part-authoring-design.md` (sub-project SP-7)
**Consumes:** SP-2 (`ScriptHost` / QuickJS-ng, seeded RNG hook), SP-3 (resolved-hash
folding, cache-miss invalidation).

## Goal

Give part scripts a **reusable, deterministic helper library** and a **module-import
mechanism** to pull it in — without breaking content-addressed caching. Two pieces:

1. **A v1 helper library** of common procedural-authoring utilities, importable from part
   scripts.
2. **Import + hashing rules** so that importing a shared module is deterministic and an edit
   to a shared module correctly invalidates every part that uses it.

## v1 library contents

A deliberately scoped helper set covering the prototype's recurring needs:

- **L-system** — rule/axiom expansion (string-rewriting) for trees, branches, growth
  structures (pairs with the skinned-line tubing from SP-6).
- **Bézier** — curve evaluation / sampling (paths, sweeps, smooth branch spines).
- **Vector / matrix math** — vec2/3/4, mat4 helpers, transforms, lerp/slerp — the
  arithmetic authors otherwise rewrite per script.
- **Geometry helpers** — primitive builders / common shape utilities layered on the DSL
  (e.g. polygon rings, lattices of points, simple solids).
- **Seeded RNG** — a deterministic PRNG seeded from params (see Determinism).

**Deferred (not in v1):** flow-field and agent-simulation utilities. They're additive and
can land later as new modules without changing the import/hash mechanism.

## Imports — explicit ES module imports

- Part scripts use **explicit ES module `import` statements** to pull helpers from a
  **shared-lib folder** (a known location outside the per-world `ObjectSchemas/` tree).
- Rationale: ES modules are first-class in QuickJS-ng; explicit imports make a script's
  dependencies legible and let the host resolve exactly which module source to hash. No
  implicit globals, no magic injection.
- The host provides a **module loader/resolver** that maps import specifiers to shared-lib
  files and feeds their source to the QuickJS-ng module system, within the same isolated
  per-bake context (SP-2 determinism contract preserved).

## Hashing — fold imported module source into the importer's resolved hash

- When a part imports shared modules, the host **folds the full source of every (transitively)
  imported module into the importing part's resolved hash.** Concretely, the part's
  `script_source_bytes` (an input to `compute_resolved_hash`, SP-1) is taken as the part
  source **plus its imported module sources** (deterministically ordered).
- Consequence: **editing a shared module changes the resolved hash of every part that
  imports it** (directly or transitively), so those parts miss the cache and rebake — exactly
  the transitive-invalidation guarantee SP-3 relies on, extended across the library boundary.
- This is the **same content-addressing principle** applied to library code: the cache key
  reflects *all* source that determines the geometry, so a stale artifact is impossible after
  a helper change. (Distinct from SP-3's *child-part* hashing, which folds child
  resolved-hashes; here we fold library *source* because library code is inlined into the
  bake, not instanced as a separate part.)

## Determinism — seeded RNG from the library

- The library provides the **seeded, deterministic PRNG** that backs the host's
  `Math.random` replacement (SP-2): no real entropy, fully reproducible.
- **Seed comes from params** — a part seeds its RNG from a value in its params object (e.g.
  `p.seed`). Because params fold into the resolved hash, the same params → same RNG stream →
  same geometry → same artifact; a different seed param → a different artifact (the basis of
  SP-6 variations).
- All library helpers that need randomness (L-system stochastic rules, scatter, jitter) draw
  from this seeded stream, so the whole library is deterministic under the bake contract.

## Architecture

```
shared-lib/                      # reusable helper modules (source-of-truth, hashed)
  lsystem.js  bezier.js  vecmath.js  geometry.js  rng.js

part script (ObjectSchemas/foo.js)
  import { lsystem } from 'shared-lib/lsystem'
  import { rng }     from 'shared-lib/rng'
  class Foo extends Part { build(p){ const r = rng(p.seed); ... } }

ScriptHost (SP-2):
  module resolver → loads shared-lib sources into the isolated context
  resolved hash   → fold(part source + transitively-imported module sources) ⊕ params ⊕ child-hashes
```

## Testing

Headless `tests/shared_lib_tests`:

- **Import resolves:** a part importing a shared module bakes successfully; the helper's
  behavior is reflected in the geometry.
- **Source-fold invalidation:** edit a shared module → importing parts get new resolved
  hashes and rebake; non-importing parts are untouched (drives SP-5's shared-module-edit
  test).
- **Transitive import fold:** module A imports module B; editing B invalidates parts that
  import A (transitive source fold).
- **Deterministic RNG:** same seed param → identical stream/geometry across bakes; different
  seed → different geometry; no real entropy (no Date/crypto source).
- **L-system / Bézier / vecmath / geometry:** unit tests for each helper's pure output
  (deterministic given inputs).
- **Ordering stability:** import order / file enumeration yields a stable folded-source
  hash (no nondeterministic ordering in the fold).

## Goals / Non-goals

**Goals**
- v1 helper library: L-system, Bézier, vector/matrix, geometry helpers, seeded RNG.
- Explicit ES-module imports from a shared-lib folder via the host's module resolver.
- Fold transitively-imported module source into the importer's resolved hash (cross-library
  transitive invalidation).
- Seeded deterministic RNG seeded from params (backs SP-2's `Math.random` and SP-6
  variations).

**Non-goals (deferred)**
- Flow-field / agent-simulation utilities (additive later modules).
- Versioned/semver library packaging or external package fetch (shared-lib is in-repo
  source, hashed by content).
- Per-module independent caching/compilation (modules are inlined into the bake and hashed
  as source; no separate module artifact).
- A large standard library beyond the v1 set.

## Open questions (resolve in planning)

- Shared-lib folder location and import-specifier scheme (bare `shared-lib/x` vs. relative
  paths) and how the resolver maps them to files.
- Canonical ordering for folding multiple imported sources (by resolved specifier? by
  load order?) so the hash is stable and reproducible.
- Whether shared modules may themselves declare/instantiate parts (`requires`) or are
  strictly pure helper code — v1 leans **pure helpers only** (parts live in
  `ObjectSchemas/`); confirm.
- PRNG algorithm choice (e.g. PCG/xoshiro) and seeding API ergonomics shared with SP-2.
