# Realtime Raytraced Lighting via OptiX

## Summary

Replace the broken probe-based lighting system with realtime raytraced lighting using NVIDIA OptiX on RTX 4090 hardware. OptiX traces shadow, GI, reflection, and SSS rays at quarter resolution, denoises with its built-in AI denoiser, and composites the result onto the existing GPU-driven raster output via CUDA-GL interop. The raster pipeline is unchanged except for adding G-buffer MRT outputs in Phase 2.

## Motivation

- Lighting probes are non-functional
- Current software BVH raytracing (GLSL fragment shader traversal) is too slow for realtime use
- RTX 4090 RT cores provide ~100x speedup over software traversal but are inaccessible from OpenGL
- OptiX provides hardware RT access with mature CUDA-GL interop for hybrid rendering

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    Frame Loop                             │
├─────────────────────────────────────────────────────────┤
│                                                          │
│  1. WorldComposer resolves instances (existing)          │
│          │                                               │
│          ├──────────────────┐                            │
│          ▼                  ▼                            │
│  2a. GpuCuller +       2b. OptiX TLAS update             │
│      Raster draw           (instance transforms)         │
│      (existing)                                          │
│          │                  │                            │
│          ▼                  ▼                            │
│  3a. Color + G-buffer  3b. Trace shadow/GI rays          │
│      (custom FBO)          (quarter res)                 │
│                             │                            │
│                             ▼                            │
│                        3c. Temporal accumulation          │
│                             │                            │
│                             ▼                            │
│                        3d. OptiX AI Denoiser             │
│                             │                            │
│                             ▼                            │
│                        3e. CUDA→GL texture interop       │
│                             │                            │
│          ┌──────────────────┘                            │
│          ▼                                               │
│  4. Fullscreen composite pass                            │
│     (albedo × RT lighting)                               │
│                                                          │
└─────────────────────────────────────────────────────────┘
```

## New Module: RtLighting

Lives in `MatterEngine3/src/render/`. Self-contained module that consumes the same instance list WorldComposer produces and outputs GL textures.

Owns:
- CUDA context + OptiX device context
- Acceleration structure management (BLAS per part, TLAS from instances)
- Ray generation programs (.cu shaders)
- CUDA-GL interop resources (registered textures)
- OptiX AI denoiser state
- Temporal accumulation buffers (history, motion)

Interface:
- `RtLighting::init()` — CUDA + OptiX setup, denoiser allocation
- `RtLighting::available()` — returns false if no NVIDIA GPU / driver too old
- `RtLighting::register_part(part_id, vertices, indices, material_ids)` — build + cache BLAS
- `RtLighting::unregister_part(part_id)` — release BLAS
- `RtLighting::update_instances(resolved_list)` — rebuild or update TLAS
- `RtLighting::trace(depth_tex, gbuffer_textures, sun_dir, camera)` — launch rays, denoise, output
- `RtLighting::get_lighting_texture()` — GL texture handle for composite shader

## Phase 1: Shadow-only proof (MatterViewer)

### Goal
Trace 1 shadow ray per pixel at quarter resolution, produce a shadow visibility mask, composite onto raster output. Zero changes to existing raster shaders.

### Data Flow

1. **Part registration** — When PartStore loads a part's mesh, it calls `RtLighting::register_part()`. OptiX builds a compacted BLAS and caches it by part ID. Same dedup logic as existing BLASManager.

2. **Per-frame instance sync** — After WorldComposer resolves instances, `RtLighting::update_instances()` rebuilds the TLAS from instance transforms. Sub-millisecond on RTX 4090 for ~10K instances.

3. **Trace** — Ray generation program launches at quarter res:
   - Reconstructs world position from raster depth buffer (shared via CUDA-GL interop on `depth_copy_tex_`)
   - Casts 1 ray toward sun direction
   - Writes binary shadow (0.0 or 1.0) to output buffer
   - Jitters ray origin slightly for soft shadow accumulation over frames

4. **Interop output** — Shadow buffer is a CUDA surface backed by a GL texture via `cudaGraphicsGLRegisterImage`. After trace, GL samples it immediately.

5. **Composite** — Fullscreen GL shader: `raster_color * mix(shadow_darkening, 1.0, shadow_tex)`. Bilinear sampling at quarter res gives soft penumbra edges.

### Depth Buffer Sharing (GL to CUDA)

The HiZ pass already blits depth to `depth_copy_tex_` (GL_DEPTH_COMPONENT32F). Register this texture with CUDA via `cudaGraphicsGLRegisterImage`. In the ray gen program, sample depth + inverse view-projection matrix to reconstruct world-space position.

### Build System

```makefile
CUDA_PATH ?= /usr/local/cuda
OPTIX_PATH ?= /opt/optix

NVCC = $(CUDA_PATH)/bin/nvcc
NVCC_FLAGS = -ptx -I$(OPTIX_PATH)/include -I$(CUDA_PATH)/include

PTX_DIR = shaders_rt
PTX_FILES = $(PTX_DIR)/shadow_raygen.ptx

$(PTX_DIR)/%.ptx: $(PTX_DIR)/%.cu
	$(NVCC) $(NVCC_FLAGS) -o $@ $<

LDFLAGS += -L$(CUDA_PATH)/lib64 -lcudart
CFLAGS  += -I$(CUDA_PATH)/include -I$(OPTIX_PATH)/include
```

### Phase 1 Result

Existing raster scene with correct hard shadows from the sun at near-zero visible latency. Shadows update in real time during camera movement.

## Phase 2: G-buffer + Full RT Lighting (MatterViewer)

### Goal
Replace all lighting (probes, baked AO, sun) with full raytraced PBR. Temporal accumulation + OptiX AI denoiser for clean 1-spp results.

### G-buffer Attachments

Add MRT outputs to raster shaders (custom FBO replaces default):

| Attachment | Format | Contents |
|-----------|--------|----------|
| RT0 | RGBA8 | Albedo (material color, no lighting applied) |
| RT1 | RGB10_A2 | World-space normal (octahedral encoded) |
| RT2 | RGBA8 | R=roughness, G=metallic, B=materialID/255, A=translucency |
| Depth | D32F | Existing depth |

Raster shader change: remove all lighting math, output surface properties only.

### Ray Generation Program (Full Lighting)

At quarter res, per pixel:
1. Reconstruct world position from depth; sample G-buffer for normal, material properties
2. **Shadow ray** toward sun — binary visibility (same as Phase 1)
3. **Indirect GI ray** — cosine-weighted hemisphere sample (1 ray per pixel)
   - On hit: read albedo from hit triangle's material, cast nested shadow ray for sun visibility at hit point
   - On miss: sample sky color
4. **Reflection ray** (if roughness < 0.3) — mirror direction + roughness-based cone jitter
5. **Translucency/SSS** (if translucency flag set) — trace through surface in reverse-normal direction, measure thickness, apply Beer's law: `transmission = exp(-thickness * sigma_t) * material_color`

Output: `float4` combined lighting per pixel (direct sun + indirect GI + reflection + transmission).

### Temporal Accumulation

- History buffer stores previous frames' accumulated lighting
- Per pixel: reproject into previous frame using depth + camera matrix delta
- Blend: `result = lerp(current_sample, history, 0.9)` (90% history weight)
- History rejection criteria:
  - Depth disocclusion: `abs(current_depth - reprojected_depth) > threshold`
  - Normal discontinuity: `dot(current_normal, reprojected_normal) < 0.8`
  - Large camera movement: flush entire history
- Convergence: ~30-60 frames when camera is still (0.5-1s at 60fps)

### OptiX AI Denoiser

After temporal accumulation:
- Input: accumulated noisy lighting + G-buffer albedo + G-buffer normals
- Output: denoised lighting buffer
- ~1ms on RTX 4090 at quarter res
- Configured with `OPTIX_DENOISER_MODEL_KIND_AOV` for best quality with auxiliary buffers

### Composite Pass

```glsl
vec3 albedo   = texelFetch(g_albedo, fullResCoord).rgb;
vec3 lighting = texture(rt_denoised, quarterResUV).rgb; // bilinear upscale
vec3 final    = albedo * lighting;
final         = final / (final + 1.0); // Reinhard tonemap
final         = pow(final, vec3(1.0/2.2));              // gamma
```

### PBR Material Model

Materials carry: albedo, roughness, metallic, translucency, SSS sigma_t. These are stored in the existing material table (64 materials x N floats) and accessible in both the raster G-buffer output and the OptiX closest-hit program via the per-triangle materialId in TriEx.

### SSS / Translucency

For leaves and thin geometry:
- G-buffer marks translucent materials via RT2.a
- Ray gen traces in reverse-normal direction through the surface
- Measures entry-to-exit thickness
- Applies Beer's law: `transmission = exp(-thickness * sigma_t) * material_tint`
- Sun light transmitting through a leaf is tinted by leaf color — backlit glow effect

### Phase 2 Result

Full PBR realtime lighting. Sharp shadows, colored indirect bounce, glossy reflections on smooth surfaces, subsurface leaf glow. Clean when camera is still, gracefully noisy during fast movement.

## Phase 3: Port to ExplorerDemo

### Goal
Same RtLighting module in ExplorerDemo's infinite streaming world with LOD-instanced-children.

### Differences from MatterViewer

| Concern | MatterViewer | ExplorerDemo |
|---------|-------------|--------------|
| Instance count | Moderate (single scene) | High (streaming sectors, 10K-50K+) |
| LOD-instanced-children | N/A | Branches expand at close range |
| Part lifecycle | Load once, stable | Stream in/out with sectors |

### Streaming TLAS Management

- Build initial TLAS with `OPTIX_BUILD_FLAG_ALLOW_UPDATE`
- When only transforms change (camera moves, same instances): `optixAccelBuild` with `OPTIX_BUILD_OPERATION_UPDATE` (refit, sub-ms)
- When instances are added/removed (sector stream): full TLAS rebuild (still fast on 4090)
- BLAS builds for newly-streamed parts run on a separate CUDA stream (async, non-blocking)

### LOD-instanced-children Interaction

When the resolver expands tree branches into individual instances at close range, those expanded instances appear in WorldComposer's resolved list. `RtLighting::update_instances()` already consumes this list — no special handling needed. The TLAS simply has more instances when near trees.

### Fallback

If OptiX initialization fails (no NVIDIA GPU, driver too old):
- `RtLighting::available()` returns false
- Raster path falls back to flat ambient + sun (existing code path minus probes)
- No crash, just basic lighting

### Phase 3 Result

Full raytraced lighting in the infinite streaming world. Shadows filter through LOD-expanded branches, indirect GI bounces green from grass, leaves glow with subsurface transmission.

## Code Removed

After all phases:
- `probe_bake.cpp` — entire probe system
- `BLASManager` GPU texture upload (tiled float textures for software traversal)
- `TLASManager` GPU texture upload
- `bvh_tlas_common.glsl` — software BVH traversal
- `raytrace_tlas_blas.fs` — fullscreen RT fragment shader
- `lighting.glsl` — rewritten as CUDA (.cu)
- Probe-related uniforms and texture slots in raster shaders

## Code Retained

- Entire GPU-driven raster pipeline (compute cull, indirect draw)
- PartStore geometry loading and LOD management
- WorldComposer / SectorLodResolver instance resolution
- Material table and per-triangle TriEx data
- HiZ occlusion culling (still needed for raster)

## Dependencies

- CUDA Toolkit (build-time: nvcc; runtime: libcudart)
- OptiX SDK 8.x (headers only; runtime in NVIDIA driver >= 535)
- RTX GPU (4090 target; any RTX card works, just slower)
- GL 4.6 (existing requirement, unchanged)

## Configuration

- `rt_scale`: trace resolution as fraction of display (default 0.25 = quarter res; adjustable at runtime for quality/perf tradeoff)
- `rt_temporal_blend`: history weight (default 0.9; lower = more responsive, noisier)
- `rt_enabled`: master toggle (runtime; falls back to flat ambient + sun when off)

## Performance Budget (RTX 4090, 1080p, quarter-res trace)

| Stage | Estimated cost |
|-------|---------------|
| TLAS update (refit) | < 0.5ms |
| Shadow rays (480x270, 1 spp) | ~0.5ms |
| GI rays (480x270, 1 spp + nested shadow) | ~1.5ms |
| Reflection rays (480x270, conditional) | ~0.5ms |
| OptiX denoiser | ~1ms |
| Composite | < 0.1ms |
| **Total RT overhead** | **~4ms** |

At ~4ms overhead on a 16ms frame budget (60fps), this leaves 12ms for raster — well within the GPU-driven pipeline's capability.
