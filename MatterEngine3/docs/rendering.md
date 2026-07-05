# MatterEngine3 Rendering & Viewer

## Headline fact

**Rasterization is the default path, and it is GPU-driven.** Every frame, a
compute shader (`cull.comp`) does per-cluster frustum + projected-size LOD
selection over the resolved instance set, appends a `DrawArraysCmd` per surviving
(part, cluster, LOD) bucket, and the CPU issues a single
`glMultiDrawArraysIndirect` with the vertex shader reading per-instance
transforms via `gl_BaseInstance` out of a live SSBO — no CPU batch walk, no
per-frame `UploadMesh`, no per-cluster `DrawMeshInstanced`. **GL 4.6 is a hard
requirement** (compute + SSBO + indirect + `gl_BaseInstance` in GLSL 460). The
software ray tracer remains available via `MATTER_RT=1` as the compatibility
escape hatch on older GL (and for offline correctness reference; note: RT mode
requires a ~60–65s GPU shader compile on first launch).

## Default raster path (GPU-driven)

### Viewer flow (`viewer/main.cpp`)

1. Raylib + ImGui init (1280×720). MSAA hint is gated OFF under the GPU-driven
   raster path (HiZ blit depends on a single-sample default FB).
2. `LocalProvider` bakes/reconciles the world's part graph (QuickJS → `.part` cache).
3. GL 4.6 gate: `viewer::gl46_available()` — if it fails, the viewer FATALs with
   a hint to set `MATTER_RT=1` (there is no CPU raster fallback). `MATTER_GPU_CULL`
   defaults ON; setting `MATTER_GPU_CULL=0` also FATALs unless paired with
   `MATTER_RT=1`.
4. `RasterComposer::init` loads `shaders/raster.vs`/`shaders/raster.fs` for uniform
   discovery; `RasterComposer::init_gpu_driven` then loads
   `shaders_gpu/raster_gpu_driven.vs` + a `#version 460`-patched copy of the
   raster fragment shader as the live draw program.
5. `GpuCuller::init` compiles `shaders_gpu/cull.comp` (and, when HiZ is enabled,
   `shaders_gpu/hiz_downsample.comp`), allocates the persistent SSBOs, and warms
   the indirect command buffer.
6. Frame loop: resolver → `GpuCuller::cull` (compute) → `RasterComposer::draw_gpu_driven`
   (single `glMultiDrawArraysIndirect`) → optional HiZ pyramid build → ImGui HUD.

### Per-frame composition (`viewer/gpu_culler.cpp`, `viewer/raster_composer.cpp`)

Every frame the compute pipeline runs:

1. `SectorResolver` (PassThrough or SectorLod) produces a flat list of
   `ResolvedInstance` records (part hash + world transform + LOD hint).
2. `GpuCuller::cull` uploads the resolved instance stream into a `GpuInstanceRec`
   SSBO (binding 1), sets uniforms (camera position, frustum planes, VP,
   pixel budget), zeros the per-bucket draw commands, and dispatches `cull.comp`.
3. Inside the compute shader each thread walks its instance's part → cluster
   expansion table (`GpuClusterMeta` SSBO, binding 0) and, per cluster:
   frustum-culls the transformed AABB, HiZ-culls against the previous-frame
   max-pyramid (when `MATTER_HIZ` is on), picks a LOD via projected size, then
   atomically appends the world transform to that (part, cluster, LOD) bucket's
   `DrawXforms` slice (SSBO binding 3) and increments the matching
   `DrawArraysCmd`'s `instance_count` (SSBO binding 2). `MATTER_HIZ_DEBUG`
   emits per-cluster HiZ probes for offline inspection.
4. `RasterComposer::draw_gpu_driven` binds `shader_gpu_`, uploads frame
   uniforms via `setup_frame_uniforms` (sun/probes/material table), computes
   MVP the raylib way (`rlGetMatrix*`), and issues one
   `glMultiDrawArraysIndirect` over the live command buffer. The vertex shader
   fetches its per-instance transform out of `DrawXforms` using
   `gl_BaseInstance + gl_InstanceID`; there is no CPU vertex re-upload, no
   per-cluster draw, and no batch-fingerprint cache.
5. After `EndDrawing`, `GpuCuller::build_hiz` blits the depth buffer, runs
   `hiz_downsample.comp` down the mip chain, and marks the pyramid valid for
   next frame's occlusion pass.

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
1280×720, GPU-driven cull: see `docs/perf/meadow_sweep.csv` for the current 5-pose
sweep (columns: label, frame_ms, resolve_ms, build_ms, draw_ms, instances_active,
raster_batches, raster_tris, culled_clusters, hiz_culled). The historical
Phase-3 CPU-batch baseline (277 batches / 8.7M tris / 94 ms, 2026-07-02
commit 9a87fdc) has been superseded by the GPU-driven `gpucull-promoted` rows;
draw_ms is single-digit ms across all standard poses. Scatter constants:
GRASS_CLUMPS=40000, BLADES default (Grass.js), kMinProjectedSize=0.0015
(Meadow: active radius 400).

### Raster vertex data (`viewer/raster_mesh.h`, `raster_mesh.cpp`)

`build_raster_mesh_data` packs each part's `Tri`/`TriEx` arrays into raylib Mesh
channels: vertices (float3), normals (N0/N1/N2 → float3 per vertex),
colors (tint RGBA → unsigned char4), texcoords (materialId in U, per-vertex AO
in V). Non-indexed; 3 vertices per triangle. On the GPU-driven path,
`GpuCuller` uploads the packed vertex arrays into per-part vertex SSBOs at
registration time; the vertex shader indexes them directly, so `UploadMesh`
is not used and there is no VAO churn per frame.

### Phase-2 world light list

`world.manifest` may contain `light` lines parsed by `world_lights::parse_lights`:

```
light sun  <dx> <dy> <dz>  <r> <g> <b>         # sun direction + linear-RGB color
light sky  <r> <g> <b>                           # sky ambient color
light spot <px> <py> <pz>  <dx> <dy> <dz>  <r> <g> <b>  <range>  <inner_deg> <outer_deg>
```

Missing lines produce defaults that reproduce the Phase-1 hardcoded look. The
`WorldLights` struct is uploaded to the raster shader each frame (`sunDir`, `sunColor`,
`ambientColor`) and to the ray-tracer (`wlSunDir`, `wlSunColor`, `wlSkyColor`).

### Phase-2 probe-volume lighting (`src/probe_bake.cpp`, `viewer/probe_texture.h`)

On world load, `LocalProvider` bakes a CPU SH-L1 probe volume covering the scene:

1. Grid is sized to the world AABB with `cell ≈ 4` world units.
2. Per probe, ray bundles sample sky/sun visibility and accumulate ambient irradiance
   (hemisphere-weighted sky + blocked sun) via the world tracer.
3. Result cached as `cache/<world>.probes` (PRB1 format; fingerprint = lights hash).
   Second run skips baking and loads from cache (no "baking probes" prints).
4. Uploaded as two RGBA8 3D textures (GL units 4/5):
   - `tex_ambient`: `rgb = clamp(irradiance / 4)`, `a = sun_vis [0,1]`
   - `tex_dominant`: `rgb = dir * 0.5 + 0.5`, `a = clamp(intensity / 4)`
5. Sampled in `raster.fs` with `useProbes=1`, trilinear, `CLAMP_TO_EDGE`.
   Adds a dominant-direction cosine term to the ambient term.
6. If probes are unavailable (file missing, world without geometry), the raster path
   falls back to flat ambient using `sky_color` from the light list.

Editing any `light` line changes the lights fingerprint → cache miss → re-bake.

### Phase-3 lighting model (`shaders/raster.vs`, `shaders/raster.fs`)

The raster fragment shader supports three ambient modes selected by `useProbes`:

| Mode | When | Ambient term |
|---|---|---|
| `useProbes=1` | probes available | trilinear sample from 3D probe textures; dominant-direction cosine term added |
| `useProbes=0` | probes OFF/missing | `ambientColor * ao` (flat ambient from `sky_color` uniform) |

Sun lighting is always applied: `sunColor * max(dot(N, -sunDir), 0)`.

Material albedo and emission from the material table uniform (64 materials × 12 floats).
Reinhard tone-map + gamma 2.2 applied per fragment.

### Fallback matrix

| Artifact present | Geometry path | Lighting path |
|---|---|---|
| `<hash>.flat.part` (v3, clusters) | per-cluster frustum cull + LOD select | probe sampling if probes valid, else flat ambient |
| `<hash>.flat.part` (v2, whole-part) | whole-part LOD via `lod_mesh_data[level]` | probe sampling if probes valid, else flat ambient |
| `<hash>.part` only (compositional) | recursive child expansion (depth ≤ 8) | probe sampling if probes valid, else flat ambient |

No v3 clusters → whole-part batch path (cluster_index = UINT32\_MAX). No flat artifact
at all → compositional recursive expansion, always LOD0 for children.

### Per-cluster LOD selection (`shaders_gpu/cull.comp`)

Per surviving cluster, the compute shader:
1. Transforms the cluster AABB center by the instance world transform.
2. Computes `projected_size = cluster.radius * scale / distance` (same formula as
   `lod_select::select_level`, kept in lockstep with the CPU reference in
   `raster_cull.h` — verified by `test_gpu_cull_parity` in `gpu_cull_tests`).
3. Picks the coarsest LOD level whose threshold ≤ projected_size (fine→coarse scan).
4. Appends the transform to the (part, cluster, LOD) bucket's `DrawXforms` slice
   and increments its `DrawArraysCmd::instance_count`.

### HiZ occlusion (`shaders_gpu/hiz_downsample.comp`, `MATTER_HIZ`)

`MATTER_HIZ=1` (default; `MATTER_HIZ=0` opts out at startup, HUD checkbox +
`hiz on|off` on the FIFO toggle at runtime) enables previous-frame HiZ
occlusion culling. After each frame the viewer blits depth into an R32F
pyramid, then `hiz_downsample.comp` walks the mip chain producing max-per-mip.
Next frame's `cull.comp` samples the four screen-space corners of each
cluster's projected AABB against the pyramid's coarsest fitting mip; clusters
whose min-z exceeds the sampled max-z are dropped. Near-plane bail, tile
clamp, and 4-corner mip coverage keep the occlusion test conservative
(no observed pixel differences on canonical poses, verified by
`test_hiz_occlusion` in `gpu_tests`). `MATTER_HIZ_DEBUG=1` writes per-cluster
HiZ probe data to a small SSBO for offline inspection.

### HUD stats (`viewer/ui.cpp`)

The Viewer Debug panel shows:
- `FPS / frame ms` — wall-clock frame time.
- `Instances: N active / M total` — instances that survived the resolver vs
  world total (pre-GPU-cull; the compute shader may further cull them).
- `GPU cull: emitted=E / culled=C / hiz=H  tris=T` — total per-cluster
  instances the compute pass emitted, per-cluster frustum + LOD kills, HiZ
  kills, and the total triangles summed from live `DrawArraysCmd`s.
- `Probes: NxNxN` or `Probes: OFF` — active probe grid dimensions, or fallback indicator.

Background sky color is derived by tone-mapping `sky_color` from the light list
(Reinhard + gamma 2.2) so it tracks the world's ambient hue.

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
