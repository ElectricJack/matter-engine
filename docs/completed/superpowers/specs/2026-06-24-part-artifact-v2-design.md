# SP-1 — Part Artifact v2 (`.part` format extension) — Design

**Status:** Approved for planning (2026-06-24)
**Project:** MatterEngine3
**Parent:** `2026-06-24-procedural-part-authoring-design.md` (sub-project SP-1)
**Extends:** `2026-06-20-part-serialization-design.md` (the v1 `.part` deep cache)

## Goal

Extend the existing `.part` artifact so it can serve as the identity and composition
unit for the procedural part system, without pulling in the script host (SP-2) or
graph orchestration (SP-3). SP-1 delivers three additions over the v1 format:

1. **Resolved-hash identity** — the cache key/identity becomes a content-addressed
   *resolved hash* folding script source + params + child resolved-hashes.
2. **Child-instance table** — a part records references to *other parts* (by resolved
   hash) plus transforms, so world assembly (SP-4) can expand a part into its children.
3. **Per-part LOD BLAS levels (format only)** — an ordered array of LOD levels the
   artifact can carry and round-trip; SP-4 fills it using `mesh_simplifier`. Each level
   carries a `screen_size_threshold` (see §LOD) so SP-4 can pick a level by projected
   screen size.

SP-1 is a **pure serialization + hash-plumbing** task. It depends only on the existing
backend (`bvh.h`, `blas_manager.hpp`, `tlas_manager.hpp`, `material_registry.h`). It is
**GL-free** and **unit-tested with synthetic byte blobs** — it never parses JS and has
no knowledge of how the source/params bytes were produced.

## Scope decisions (settled during brainstorming)

- **Self-describing depth = identity + composition only (lean).** The `.part` embeds its
  resolved hash (identity/integrity) and the child-instance table (for composition) but
  **not** source/params provenance. The script files remain the source-of-truth for
  hashing; dev-mode (SP-5) recomputes hashes from source. Full provenance can be added
  later as an appended section (the v1 spec's deferred "embedded params" plan) when
  replication (SP-8) needs it.
- **Two separate instance sections.** The v1 internal-instance section (local-BLAS
  composition of the part's own geometry) is kept unchanged; the child-instance table is
  a new, distinct section. They are read by different layers and use different reference
  types (local `u32` index vs. `u64` resolved hash), so they stay separate.
- **LOD = format-only in SP-1.** SP-1 reserves and round-trips an ordered LOD-level array;
  SP-4 (which owns "per-part LOD BLAS at bake") generates the decimations. SP-1 tests
  exercise the empty / single-level case.
- **LOD representation = ordered level array referencing BLAS entries.** Each level lists
  the BLAS-table indices that constitute the whole part at that detail — handles
  multi-BLAS parts (e.g. per-material BLASes) and matches "a part has N levels."
- **LOD selection is screen-size driven.** Each level carries a `screen_size_threshold`
  (projected pixel/normalized-screen extent). SP-4 picks the coarsest level whose
  threshold is satisfied for a sector (the sector decides by its closest instance's
  projected size — see the SP-4 spec). SP-1 only stores/round-trips the float; it makes
  no selection.
- **Clean v2 cutover, no v1 back-compat.** Bump `format_version` to 2; v1 files fail
  validation and regenerate via the existing regenerate-on-mismatch contract. No v1
  reader retained (v1 is throwaway brick-dev cache).
- **Materials section unchanged** — snapshot + validate against the live registry.

## Resolved hash

```
resolved_hash = fnv1a64( script_source_bytes
                       ⊕ params_bytes
                       ⊕ sorted(child_resolved_hashes) )
```

- SP-1 owns the helper that computes it and treats all three inputs as **opaque byte
  ranges**. The orchestrator (SP-3) supplies them; SP-1 never interprets them.
- Child hashes are sorted before folding so the hash is order-independent over children.
- `resolved_hash` is the part's identity, the cache key, and the filename stem.
- The stored header field is `resolved_hash ^ format_version` (the v1 version-guard trick
  carried forward), so a format bump invalidates all cached files.

### API (additions to `part_asset.h`)

```cpp
// Content-addressed identity. All three inputs are opaque byte ranges to SP-1.
// child_hashes need NOT be pre-sorted; the helper sorts internally.
uint64_t compute_resolved_hash(const void* source_bytes, size_t source_len,
                               const void* params_bytes, size_t params_len,
                               const uint64_t* child_hashes, size_t child_count);

// Child-instance record: a reference to another part by resolved hash + placement.
struct ChildInstance {
    uint64_t child_resolved_hash;
    float    transform[16];   // row-major, world placement under the parent's frame
};

// v2 save/load. The internal BLAS/internal-instance/material sections match v1's
// shape; v2 appends the child-instance table and the LOD-level array.
bool save_v2(const std::string& path,
             const BLASManager& blas, const TLASManager& tlas,
             const ChildInstance* children, size_t child_count,
             const LodLevels& lods,            // see §LOD; may be empty/single-level
             uint64_t resolved_hash);

bool load_v2(const std::string& path, uint64_t expected_resolved_hash,
             BLASManager& blas, TLASManager& tlas,
             std::vector<ChildInstance>& children_out,
             LodLevels& lods_out);
```

`cache_path()` keys on the resolved hash: `parts/<resolved_hash-16hex>.part`.

## On-disk format (v2)

Sections are fixed-order and length-prefixed (each starts with its count), so v2 appends
trailing sections onto v1 without disturbing the existing ones.

```
Header
  magic            u32  'PART' (0x50415254)
  format_version   u32  = 2
  resolved_hash    u64  (stored as resolved_hash ^ format_version)   // cache key / identity
  sizeof_Tri       u32                                               // layout guards
  sizeof_TriEx     u32
  sizeof_BVHNode   u32
  sizeof_ChildInstance u32                                           // new guard
  content_hash     u64  FNV-1a over all bytes after header

Materials          [unchanged from v1]
  count            u32
  MaterialDef[count]

BLAS table         [unchanged from v1]
  blas_count       u32
  per BLAS: hash u32, ref_count u32, tri_count u32, nodes_used u32,
            Tri[], TriEx[], BVHNode[], triIdx[]

Internal instances [unchanged from v1 — local-BLAS composition of this part]
  inst_count       u32
  per instance: blas_index u32, material_id u32, transform f32[16]

Child instances    [NEW — references to other parts for world composition]
  child_count      u32
  per child: child_resolved_hash u64, transform f32[16]

LOD levels         [NEW — ordered, may be empty]
  level_count      u32
  per level:
    screen_size_threshold f32                // projected screen extent for selection (SP-4)
    blas_index_count u32
    blas_index       u32[blas_index_count]   // indices into the BLAS table above
```

### Validation on load (any failure → ignore file, regenerate, overwrite)

Carries forward all v1 checks, plus:
- `format_version` != 2 (v1 files fail here → clean cutover).
- `sizeof_ChildInstance` != current `sizeof(ChildInstance)`.
- recomputed `content_hash` mismatch (now covers the two new sections).
- header `resolved_hash` != requested (defense in depth; filename already encodes it).

## What is rebuilt vs. restored on load

Unchanged from v1:
- **BLAS geometry + BVH:** fully restored via `register_prebuilt(...)`, no rebuild.
- **Internal TLAS:** rebuilt from the internal-instance records (cheap) + `build()`.
- **GPU data textures:** re-packed on load via `ensure_gpu_textures_ready()`.

New, and both **passive on load** (no backend action in SP-1):
- **Child-instance table:** returned to the caller via `children_out`; SP-4 expands it
  into the world TLAS. SP-1 only round-trips it.
- **LOD levels:** returned via `lods_out`; SP-4 selects/generates levels. SP-1 only
  round-trips the index arrays.

## Testing

A headless `tests/part_asset_v2_tests` extending the v1 suite:

- **Round-trip (full):** synthetic `BLASManager` + internal instances + materials + a
  child-instance table (a few `(hash, transform)` rows) + a 2–3 level LOD array; save,
  load into fresh state, assert every section's bytes match and `content_hash` matches.
- **Round-trip (degenerate LOD):** empty LOD array and single-level LOD array both
  round-trip correctly.
- **Resolved-hash helper:** `compute_resolved_hash` is deterministic, order-independent
  over `child_hashes` (shuffled inputs → same hash), and changes when any of
  source/params/a child hash changes.
- **Layout guard:** corrupt `sizeof_ChildInstance` / `format_version` → load rejects.
- **Corruption guard:** flip a byte in the child-instance or LOD section → `content_hash`
  rejects.
- **v1 cutover:** a v1-version file (format_version=1) is rejected by the v2 loader.
- **prebuilt vs built parity:** carried over from v1.

## Goals / non-goals

**Goals**
- `compute_resolved_hash` helper (opaque-byte inputs, sorted children).
- v2 format: child-instance section + ordered LOD-level section, length-prefixed/appended.
- `save_v2`/`load_v2` round-trip, GL-free, synthetic-blob unit tests.
- Resolved hash as cache key + filename; clean v2 cutover.

**Non-goals (deferred)**
- Generating LOD decimations (SP-4 via `mesh_simplifier`).
- Expanding child instances into the world TLAS (SP-4).
- Embedding source/params provenance in the artifact (later, for SP-8 replication).
- Any script-host / DAG / orchestration logic (SP-2 / SP-3).
- Scene-level TLAS serialization (still out of scope, per v1 spec's open door).

## Open questions (resolve in planning)

- Whether `LodLevels` is a standalone struct or a `std::vector<std::vector<uint32_t>>`
  typedef — purely an API-ergonomics call for the implementer.
- Whether the internal-instance section and child-instance section should share a
  transform encoding helper (both are `f32[16]` row-major) — trivial dedup.
- Confirm `register_prebuilt` from the v1 spec needs no signature change for v2 (expected
  none — BLAS storage is unchanged).
