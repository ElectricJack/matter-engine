# SP-3 — Part Graph, Build-as-Cache-Miss & Install — Design

**Status:** Approved for planning (2026-06-24)
**Project:** MatterEngine3
**Parent:** `2026-06-24-procedural-part-authoring-design.md` (sub-project SP-3)
**Consumes:** SP-1 (`.part` v2, resolved hash) and SP-2 (single-part bake via `ScriptHost`).

## Goal

Turn a set of part scripts into a **baked part cache** by resolving the dependency graph
and baking each unique part exactly once. SP-3 owns:

1. **Dependency discovery** — read each part's `static requires` to learn its children.
2. **Resolved-hash assembly** — fold child resolved-hashes into each part's identity
   (transitive content addressing), so any change ripples upward automatically.
3. **Build-as-cache-miss** — a part is (re)baked **iff** its `parts/<resolved_hash>.part`
   is absent. Present ⇒ skip. This is the entire incremental-build mechanism.
4. **Install** — run the whole reachable graph for a world to a populated on-disk cache.

SP-3 is the **orchestrator** above SP-2: SP-2 bakes one part given its source/params/child
hashes; SP-3 decides *which* parts to bake, *in what order*, and *with what child hashes*.

## Dependency discovery — `requires` via top-level eval

- A part declares children in `static requires` (see SP-2's `Part` base class).
- SP-3 obtains `requires` by **evaluating the module's top level and reading the static
  field** — it does **not** call `build()`. Discovery is cheap and side-effect-free;
  geometry baking (the expensive `build()`) happens only on a confirmed cache miss.
- `requires` entries name child part **modules**; parameterization is per the parent's
  call (next section).

## Parameterization & dedup

- A parent instantiates a child by passing a **params object**; the child's identity is its
  `resolved_hash`, which already folds `params`. So **two instantiations with identical
  params collapse to one cached artifact** (content-hash dedup); differing params produce
  distinct artifacts.
- This is the same params→hash contract SP-2 uses; SP-3 simply supplies each child's
  params when computing that child's resolved hash, then hands the parent the child's
  resolved hash to fold into the parent's own hash.

## Resolved-hash assembly (bottom-up)

Because a parent's `resolved_hash` includes its children's resolved hashes, hashes must be
computed **leaves-first**:

```
resolve(part, params):
    child_hashes = []
    for (childModule, childParams) in part.requires(params):
        child_hashes.append( resolve(childModule, childParams) )   // recurse, memoized
    return compute_resolved_hash(part.source, serialize(params), child_hashes)   // SP-1
```

- Memoize `resolve(...)` results within an install run so a part shared by many parents is
  resolved once.
- Transitive invalidation is automatic: edit a leaf → its hash changes → every ancestor's
  folded hash changes → ancestors miss the cache and rebake. No explicit dependency
  tracking/timestamps needed.

## Build order — single-threaded topological

- Bake in **topological order, single-threaded** for v1: resolve the graph, then bake any
  missing part only after all its children exist in the cache (children first).
- Single-threaded keeps determinism and error reporting simple; parallel baking is a
  deferred optimization (the content-addressed cache makes it safe to add later).

## Cycle handling — detect and hard-error

- A part graph **must be a DAG**. SP-3 detects cycles during resolution (a back-edge to a
  node currently on the resolution stack) and **fails the install with a hard error**
  naming the cycle path. No silent break, no depth cap as a cycle workaround.

## Reachability — bake only what the world uses

- Installation is rooted at a **world's root part(s)**. SP-3 bakes the **reachable set from
  the root only** — parts not referenced (transitively) by the world are never baked.
- This bounds install cost to what the world actually needs and matches the prototype's
  per-world schema layout.

## On-disk layout (mirrors the prototype)

```
WorldData/<world>/
  ObjectSchemas/        # the world's part scripts (source of truth, hashed)
    *.js
  parts/                # the baked content-addressed cache (SP-1 artifacts)
    <resolved_hash-16hex>.part
```

- `ObjectSchemas/` holds the **scripts** (hashed as source). `parts/` is the **cache**,
  keyed by resolved hash (SP-1 `cache_path()`), safe to delete/regenerate at any time.
- Shared script-library modules (SP-7) live outside the per-world tree; their source is
  folded into importer hashes (see SP-7), so editing a shared module invalidates dependent
  artifacts the same way.

## Install flow

```
install(world):
    roots = discover world root part(s)
    resolve(roots...)            # build full resolved-hash graph, detect cycles
    for part in topo_order(reachable_from(roots)):
        if not exists(cache_path(part.resolved_hash)):
            ScriptHost.bake(part.source, part.params, part.child_hashes)   # SP-2
    # cache now complete for the world; SP-4 composes it into the world TLAS
```

## Error handling

- **Cycle detected** → hard error (cycle path).
- **Missing `requires` target** (a named child module not found) → hard error.
- **Child bake fails** (SP-2 fail-closed) → propagate; parent is **not** baked (its child
  hash is unavailable), install fails with the failing part identified.
- Install is **all-or-nothing per run** for correctness: a partial cache is fine on disk
  (content-addressed, each file valid in isolation) but the install reports failure so the
  world isn't treated as ready.

## Testing

Headless `tests/part_graph_tests` (drives SP-2's host with synthetic scripts):

- **Cache miss → bake; hit → skip:** first install bakes N parts; second install with an
  untouched cache bakes 0 (all hits).
- **Transitive invalidation:** edit a leaf script → re-resolve → leaf + all ancestors miss
  and rebake; unrelated branches stay hits.
- **Dedup:** two parents instantiate the same child with identical params → one artifact;
  with differing params → two artifacts.
- **Topo order:** children always exist before a parent is baked (instrument bake order).
- **Cycle:** a→b→a graph → hard error naming the cycle, nothing baked.
- **Reachability:** an orphan part in `ObjectSchemas/` not referenced by the root is never
  baked.
- **Failure propagation:** a child that fails to bake leaves its parent unbaked and the
  install reporting failure with the right part named.

## Goals / Non-goals

**Goals**
- `requires` discovery via top-level eval (no `build()`); memoized bottom-up resolve.
- Build-as-cache-miss as the sole incremental mechanism; transitive invalidation for free.
- Single-threaded topo-order bake; cycle detect + hard error; reachable-from-root only.
- `WorldData/<world>/ObjectSchemas/` + `parts/` layout; install flow + error propagation.

**Non-goals (deferred)**
- Parallel/multi-threaded baking.
- Composing the cache into the world TLAS / LOD selection (SP-4).
- Live re-bake on file change (SP-5).
- Shared-library module *resolution* mechanics (SP-7 owns the import + hash-fold rules;
  SP-3 only relies on importer-source hashing for invalidation).

## Open questions (resolve in planning)

- Params serialization for hashing (`serialize(params)`): stable canonical form (sorted
  keys, fixed number formatting) so equal params always hash equally.
- Whether root-part discovery is a manifest file in `WorldData/<world>/` or a convention
  (e.g. a named root module) — pick one explicitly.
- Memoization key: `(module identity, canonical params)` — confirm module identity is the
  script source hash, not a path (so a moved script with identical source is one node).
