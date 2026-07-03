# MatterEngine3 Rendering & Viewer

## Headline fact

**Rasterization is the default path.** Every frame, the viewer calls
`DrawMeshInstanced` (raylib) over flat per-LOD CPU vertex arrays built from the
engine's part graph. No shader warm-up, no ray traversal — a clean raster frame
runs in single-digit milliseconds. The software ray tracer remains available via
`MATTER_RT=1` for offline baking and correctness reference (note: RT mode
requires a ~60–65s GPU shader compile on first launch).

## Default raster path

### Viewer flow (`viewer/main.cpp`)

1. Raylib + ImGui init (1280×720, MSAA 4×). Camera defaults applied via
   `renderer.init_camera()` (used in both raster and RT mode).
2. `LocalProvider` bakes/reconciles the world's part graph (QuickJS → `.part` cache).
3. `RasterComposer::init` loads `shaders/raster.vs` / `shaders/raster.fs` and
   caches uniform locations (`sunDir`, `sunColor`, `ambientColor`, `materialTable`,
   `materialCount`). No warm-up step.
4. Frame loop: resolver → `build_batches` → lazy `UploadMesh` → `DrawMeshInstanced`
   → ImGui HUD with raster batch/tri stats.

### Per-frame composition (`viewer/raster_composer.cpp`)

Every frame:
1. `SectorResolver` (PassThrough or SectorLod) produces a flat list of
   `ResolvedInstance` records with part hash + world transform + LOD level.
2. `RasterComposer::build_batches` walks that list recursively (depth ≤ 8,
   ≤ 200k instances cap). Geometry-less assembly parts recurse without emitting;
   parts with `lod_mesh_data` get their transform appended to the matching
   `RasterBatch` (keyed by `(hash<<4)|level`).  Children always render at LOD 0.
3. `RasterComposer::draw` iterates batches: `ensure_mesh` lazy-uploads each
   (hash, level) pair to VRAM via `UploadMesh` on first access (cached for the
   lifetime of the `RasterComposer` instance). One `DrawMeshInstanced` call per
   batch; backface culling disabled (mesh winding is not guaranteed consistent).

### Root expansion (`expand` manifest flag)

A world-manifest root may carry the `expand` flag (`Meadow expand`). The
provider then does NOT place the root itself; after install it reads the root's
baked child-instance table and emits one world instance per child. Children
thereby become root instances: SectorLod selection, bake-time flattening (one
flat artifact per unique child hash), floor cull, and instanced raster batching
all apply per child. Unflagged roots place at the origin and flatten whole.

### Projected-size floor cull

`select_sector_lods` takes a `min_projected_size` (default 0 = off). Parts
whose projected size (`bound_radius / distance`) in a sector falls below the
floor are assigned level -1 and the resolver emits nothing for them — small
parts (grass, pebbles) self-cull at distance even though their error-bounded
LOD ladders stop above 1 px. The viewer enables this per world
(Meadow: 0.0015 ≈ 1 px at 720p, active radius 400).

### Meadow benchmark (Phase 3 raster baseline)

`MATTER_WORLD=meadow`, default camera `MATTER_CAM="128,25,40,128,2,128"`,
1280×720: 277 batches / 8,685,895 tris, ~1 FPS / 94 ms frame
(recorded 2026-07-02, commit 9a87fdc). Scatter constants: GRASS_CLUMPS=40000,
BLADES default (Grass.js), kMinProjectedSize=0.0015 (Meadow: active radius 400).
Note: frame ms measured on RTX 4090 via WSL/Mesa (D3D12 translation); native
GL performance will differ.

### Raster vertex data (`viewer/raster_mesh.h`, `raster_mesh.cpp`)

`build_raster_mesh_data` packs each part's `Tri`/`TriEx` arrays into raylib Mesh
channels: vertices (float3), normals (N0/N1/N2 → float3 per vertex),
colors (tint RGBA → unsigned char4), texcoords (materialId in U, per-vertex AO
in V). Non-indexed; 3 vertices per triangle. CPU arrays stay owned by `PartStore`;
`ensure_mesh` detaches the pointers after `UploadMesh` so raylib's `UnloadMesh`
cannot double-free them.

### Phase-1 lighting model (`MatterSurfaceLib/shaders/raster.vs`, `raster.fs`)

Fixed-constant, unshadowed, single sun:

- Sun direction: `(-0.45, -0.80, -0.35)` normalized (baked in `raster_composer.cpp`).
- Sun color: `(2.2, 2.05, 1.8)` — warm white.
- Ambient: `(0.38, 0.43, 0.52)` — cool blue-grey sky ambient.
- Per-fragment:
  1. Tint blend: `albedo = mix(albedo, tint.rgb, tint.a)` (from vertex `fragTint`).
  2. Lighting: `color = albedo * (ambientColor * ao + sunColor * ndl) + albedo * emission`
     where `ndl = max(dot(N, -sunDir), 0)` and `ao` is the per-vertex AO from texcoord V.
  3. Reinhard tone-map: `color = color / (color + 1)`.
  4. Gamma 2.2: `color = pow(color, 1/2.2)`.
- Material albedo and emission from the material table uniform (64 materials × 12 floats,
  same `material_registry` data used by the ray tracer).
- AO packed in texcoord V multiplies the ambient term.

No shadows, no GI, no reflections. Phase 2 will replace the fixed constants with a
light list; Phase 3 will add clustered multi-draw and v3 part format.

### HUD stats (`viewer/ui.cpp`)

The Viewer Debug panel shows:
- `FPS / frame ms` — wall-clock frame time.
- `Instances: N active / M total` — instances drawn this frame vs world total.
- `Raster: N batches / M tris` — number of `DrawMeshInstanced` calls and total
  triangle count for the frame (0/0 when `MATTER_RT=1`).

Background: blue-grey placeholder sky (`ClearBackground` with RGB 96,118,143).

---

## Reference ray tracer (`MATTER_RT=1`)

Set `MATTER_RT=1` to switch to the software ray tracer. This path is a
**reference renderer**: physically-based shadows, GI, and refraction; suitable for
offline screenshots and correctness oracles. It is not on a trajectory to real-time
frame rates without hardware RT or major restructuring.

**~60–65s warm-up** is required on first launch while the GPU compiles the
fragment shader (`raytrace_tlas_blas_processed.fs`). The `MATTER_CMD_FIFO`
live-command pipe exists to amortize this cost by keeping the viewer alive across
shots (see below).

### Viewer flow (`viewer/main.cpp`, RT branch)

1. Raylib + ImGui init. `renderer.init_camera()` and `renderer.init_shader(...)` load
   `shaders/raytrace_tlas_blas_processed.fs` (include-flattened at build time by
   `shader_preprocessor`; run `make regen-processed-shader` after editing any `.glsl`).
2. `LocalProvider` bakes/reconciles the part graph.
3. Instance count is **pre-expanded recursively** (mirroring composer depth cap 8)
   to size the TLAS before the first frame.
4. Shader warm-up: 1×1 offscreen draw + `glFinish()` — this is the ~60–65s cost.
   Only the RT path pays this; raster mode skips it entirely.
5. Frame loop: resolver → composer → single fullscreen raytrace pass → optional
   screenshot with `.done` marker.

### Per-frame composition (`viewer/world_composer.cpp:18`)

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

### GPU data layout

All geometry lives in tiled float textures (width 8192, `texelFetch` addressing):

| Texture | Layout |
|---|---|
| triangles | 6 rows/tri: v0,v1,v2,N0,N1,N2; tint RGB in spare .w of rows 1-3; AO 3x8-bit in row 5 .w |
| blasNodes | 3 rows/node: aabbMin+leftFirst, aabbMax+triCount |
| tlasNodes | 3 rows/node: aabbMin+leftChild, aabbMax+blas, rightChild |
| instances | 8 rows/instance: world + inverse-world 4x4 (re-uploaded every frame) |

Materials: uniform float array, 64 materials x 12 floats (`materials.glsl:29`).

### Shader work per pixel (`raytrace_tlas_blas.fs`, `lighting.glsl`)

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

### LOD at render time (RT path)

- **Flat-preferred**: when `<hash>.flat.part` exists, `PartStore::load_flat` registers
  its **stored** ε-bounded LOD ladder directly (no re-bake); TriEx (materials/tint/AO)
  is present on **every** level, and the thresholds come from the file.
- Fallback (non-flattened parts): `.part` stores LOD0 geometry; `get_or_load` re-runs
  `lod_bake` on load to produce LOD1 (10%) and LOD2 (1%) BLASes
  (`viewer/part_store.cpp`). These ratio-pyramid LODs lose per-triangle TriEx.
- LOD selection happens only for **root** instances via SectorLod; expanded children
  (fallback path only) are always LOD0.

### Why RT is slow

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

### Live-command FIFO (`MATTER_CMD_FIFO`)

To avoid paying the ~60–65s RT warm-up repeatedly, keep the viewer alive and drive it
via a named pipe:

```
MATTER_CMD_FIFO=/tmp/viewer.fifo MATTER_RT=1 ./viewer &
echo "cam 20 16 34 0 9 0" > /tmp/viewer.fifo
echo "shot /tmp/out.png"   > /tmp/viewer.fifo
echo "quit"                > /tmp/viewer.fifo
```

Commands (one per line): `cam <px> <py> <pz> <tx> <ty> <tz>`, `shot <path>`,
`reload`, `quit`. After `shot`, the viewer touches `<path>.done` so a driver can
poll for a fully-written file.
