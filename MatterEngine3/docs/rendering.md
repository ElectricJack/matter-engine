# MatterEngine3 Rendering & Viewer

## Headline fact

**There is no rasterization path.** Every pixel is produced by software ray tracing in a
GL 3.3 fragment shader: a fullscreen quad whose fragment shader walks a TLAS→BLAS BVH
stored in textures. Shadows, GI, reflections — all traced. No RT cores, no compute, no
screen-space approximations.

## Viewer flow (`viewer/main.cpp`)

1. Raylib + ImGui init (1280x720), load `shaders/raytrace_tlas_blas_processed.fs`
   (include-flattened by `shader_preprocessor` at build time — raylib can't handle
   `#include`; run `make regen-processed-shader` after editing any `.glsl`).
2. `LocalProvider` bakes/reconciles the world's part graph (QuickJS → `.part` cache).
3. Instance count is **pre-expanded recursively** (mirroring composer depth cap 8) to
   size the TLAS before the first frame (`main.cpp:89-102`).
4. Shader warm-up: 1x1 offscreen draw + `glFinish()` — this is the ~60s cost that the
   `MATTER_CMD_FIFO` live-command pipe exists to avoid (`cam`/`shot`/`reload`/`quit`).
5. Frame loop: resolver → composer → single fullscreen raytrace pass → optional
   screenshot with `.done` marker.

## Per-frame composition (`viewer/world_composer.cpp:18`)

Every frame:
1. `SectorResolver` picks visible root instances + LOD
   (`PassThrough` = always LOD0; `SectorLod` = 16-unit sector grid, 64-unit active
   radius, projected-size thresholds).
2. A **flattened root** (its `<hash>.flat.part` was loaded flat-preferred) emits exactly
   ONE TLAS leaf at its selected LOD — its child table is empty by construction. Only
   non-flattened parts go through recursive `emit()` child expansion (depth ≤ 8,
   ≤ 200k instances; children always at LOD 0).
3. FNV-1a fingerprint over the (blas_handle, transform) instance set: if unchanged from
   the previous frame, the TLAS rebuild is **skipped entirely**. Otherwise
   `tlas_.clear(); draw_batch(insts); build(...)` rebuilds from scratch.

`DrawInstance` already carries an `is_imposter` flag (currently always false) — the
integration point for the voxel-box imposter work in MatterSurfaceLib.

## GPU data layout

All geometry lives in tiled float textures (width 8192, `texelFetch` addressing):

| Texture | Layout |
|---|---|
| triangles | 6 rows/tri: v0,v1,v2,N0,N1,N2; tint RGB in spare .w of rows 1-3; AO 3x8-bit in row 5 .w |
| blasNodes | 3 rows/node: aabbMin+leftFirst, aabbMax+triCount |
| tlasNodes | 3 rows/node: aabbMin+leftChild, aabbMax+blas, rightChild |
| instances | 8 rows/instance: world + inverse-world 4x4 (re-uploaded every frame) |

Materials: uniform float array, 64 materials x 12 floats (`materials.glsl:29`).

## Shader work per pixel (`raytrace_tlas_blas.fs`, `lighting.glsl`)

- Path depth `MAX_DEPTH = 2` (primary + one reflect/refract bounce).
- Per primary hit: 1 shadow ray (`lighting.glsl:122`) + 1 sky-biased indirect ray
  (`sampleCount = 1`, ray length clamped to 4.0) + Cook-Torrance PBR + baked AO.
- TLAS traversal: iterative, 64-entry stack, near-child-first ordering
  (`bvh_tlas_common.glsl:461`). Per TLAS leaf, ray is transformed into instance space
  and the BLAS is walked. Shadow rays use a cheap any-hit path (`shadowQuery`,
  `bvh_tlas_common.glsl:604`).
- Miss → procedural sky.

So the floor is ~3 BVH traversals per pixel (primary + shadow + indirect), each of which
may visit many TLAS leaves. With hundreds of leaf instances per tree, TLAS breadth — not
triangle count — dominates traversal cost.

## LOD at render time

- **Flat-preferred**: when `<hash>.flat.part` exists, `PartStore::load_flat` registers
  its **stored** ε-bounded LOD ladder directly (no re-bake); TriEx (materials/tint/AO)
  is present on **every** level, and the thresholds come from the file.
- Fallback (non-flattened parts): `.part` stores LOD0 geometry; `get_or_load` re-runs
  `lod_bake` on load to produce LOD1 (10%) and LOD2 (1%) BLASes
  (`viewer/part_store.cpp`). These ratio-pyramid LODs lose per-triangle TriEx.
- LOD selection happens only for **root** instances via SectorLod; expanded children
  (fallback path only) are always LOD0.

## Why this is slow, precisely

1. **CPU TLAS rebuild on change** — the fingerprint skip removes the rebuild for static
   frames, but any instance-set change (LOD switch, movement) still triggers a full
   O(n log n) rebuild from scratch rather than a refit.
2. **Non-flattened hierarchies = fat TLAS.** A part without a flat artifact expands to
   one TLAS leaf per subtree node; rays pay instance-transform + BLAS-entry cost per
   overlapping leaf AABB, and foliage AABBs overlap heavily — the worst case for TLAS
   traversal. Flattened roots avoid this (one leaf per root).
3. **Software traversal in a fragment shader**: high register pressure (64-deep stack,
   matrices), horrible warp divergence on foliage, no hardware BVH units.
4. **All lighting is traced**: even "cheap" frames pay shadow + GI traversals per pixel.
5. **Full per-frame instance matrix re-upload** (32 floats x instances).

## What it's good for

This path is a **reference renderer**: physically-based, handles refraction/mirrors,
zero rasterization complexity, great for offline screenshots and correctness oracles.
It is not on a trajectory to "hyper-detailed at incredibly fast frame rates" — that
requires either hardware RT (Vulkan) or, more in line with the engine's premise,
rasterizing the baked instances and keeping rays for baking/sparse effects.
