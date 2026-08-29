# Temporal Rendering Foundation — Design

**Status:** Approved for planning (2026-06-24)
**Context:** Performance work on the GLSL fragment-shader ray tracer
(`raytrace_tlas_blas.fs` + `bvh_tlas_common.glsl`). Complements, and is independent of, the
sector-LOD work (`2026-06-24-sector-lod-instanced-world-design.md`). This spec builds the
shared temporal infrastructure that powers **two** consumers: temporal accumulation
(amortize expensive lighting over frames) and temporal upsampling / TAAU (render at reduced
resolution and reconstruct full resolution over frames — a DLSS-shaped result built entirely
in-engine, no vendor SDK).

## Goal

Make the raytracer faster and cleaner by **spreading lighting work across frames** and
**rendering fewer pixels per frame**, using one shared temporal foundation. Concretely:
render to offscreen history buffers, vary samples per frame, reproject the previous frame
using camera motion, and accumulate — then extend the same machinery to accumulate
reduced-resolution jittered frames into a full-resolution image.

## Why this engine is a good fit

- **Geometry is static and the light is static** (`lightPos`/`lightColor`/`ambient` are
  compile-time constants, `raytrace_tlas_blas.fs:21-23`). So reprojection needs **no
  per-object motion vectors** — the only thing that moves is the camera, and reprojection is
  driven entirely by the previous-frame camera matrices. This removes the single hardest
  part of textbook TAA.
- **It's a ray tracer, so we get linear hit distance (`hit.t`) for free.** A rasterizer must
  build a G-buffer to know world position; here every primary ray already knows its hit
  distance. We pack that scalar into the framebuffer alpha and reconstruct world position as
  `cameraPos + rayDir * t` — which means **no MRT / multi-attachment FBO is required**,
  sidestepping raylib's single-color-attachment `RenderTexture2D` limitation.

## Current state (what exists / what's missing)

- Single-pass fragment shader rendering **straight to the screen**, one sample per pixel, no
  anti-aliasing (`raytrace_tlas_blas.fs:261`).
- RNG `seed` is derived from **pixel coordinates only** (`:254`), no frame counter — the
  image is bit-identical every frame, so nothing converges today.
- No offscreen FBO, no history buffer, no reprojection, no jitter.
- A vestigial unused `LightCache`/`getGridHash` (`:26-36`) — not used by this design.
- raylib/rlgl + **OpenGL 3.3** (no compute shaders — rules out a self-built ML upscaler;
  this design uses only fragment-shader passes).

## Tech Stack

C++17, raylib/rlgl + OpenGL 3.3, GLSL 330 fragment shaders. raylib `RenderTexture2D`
(`LoadRenderTexture`) for offscreen targets; ping-pong by swapping two render textures. All
passes are full-screen fragment shaders (no compute).

---

## Architecture: the shared foundation

Three reusable pieces, consumed by accumulation and upsampling alike:

### 1. Offscreen render + history ping-pong
- The raytrace pass renders into an offscreen `RenderTexture2D` (RGBA16F preferred for HDR
  pre-tonemap accumulation) instead of the backbuffer.
- Two **history** render textures, swapped each frame (read previous / write current).
- A final **present** pass tonemaps + blits the resolved buffer to the screen. (Tone mapping
  moves out of the trace shader to the present pass so accumulation happens in linear HDR.)

### 2. Per-frame variation
- New `uint frameCount` uniform folded into `WangHash` so each frame draws a *different*
  sample (required for accumulation to converge).
- A sub-pixel **jitter** offset uniform (Halton(2,3) sequence) added to the primary ray's
  screen UV. Jitter is what lets accumulation resolve sub-pixel detail (anti-aliasing) and is
  the mechanism upsampling reuses.

### 3. Reprojection + history blend (resolve pass)
- The trace pass writes `color.rgb` and packs **linear hit distance `t`** into `color.a`.
- The resolve pass, per pixel: reconstruct current world position `P = cameraPos +
  rayDir(uv) * t`; project `P` with the **previous-frame view-projection** to get the
  previous screen UV; sample the previous history there.
- **Disocclusion / rejection:** keep a small geometry history (previous frame's packed `t`
  per pixel) and reject the reprojected sample when reconstructed positions diverge beyond a
  threshold, or when the previous UV falls offscreen. Rejected pixels restart accumulation
  from the current sample (noisy for a few frames at newly revealed edges — acceptable).
- **Blend:** exponential moving average weighted by a per-pixel accumulated-sample count
  (stored in a history alpha channel), clamped to a max history length to bound ghosting.

---

## Phasing (each phase is independently shippable and de-risks the next)

### Phase 0 — Offscreen plumbing + still-camera accumulation
- Render the trace pass into an offscreen FBO; add `frameCount` to the seed; add a present
  pass that tonemaps to screen.
- Accumulate a running average **only while the camera is unchanged**; reset the accumulator
  the instant any camera uniform changes. **No reprojection, no geometry history, no
  disocclusion** (skips foundation piece 3).
- Result: a clean, converged image whenever the camera is stationary (progressive
  refinement). Validates the entire buffer/ping-pong/seed pipeline with minimal risk.

### Phase 1 — Reprojection (accumulation survives camera motion)
- Add the packed-`t` output, the geometry history, the previous view-projection uniform, and
  the resolve pass with reprojection + disocclusion (foundation piece 3).
- Add jitter so the converged image is also anti-aliased.
- Result: full temporal accumulation — stays clean while the camera moves; secondary
  lighting (shadows/GI/AO) can drop toward ~1 sample/frame and converge over time.

### Phase 2 — Temporal upsampling (TAAU)
- Render the trace pass at a **reduced internal resolution** (e.g. 50–67%) with per-frame
  jitter; accumulate the jittered low-res samples into the **full-resolution** history via the
  same reproject-and-blend resolve pass.
- Result: the headline perf win — the shading-bound tracer runs at far fewer pixels while the
  temporal resolve reconstructs full-resolution detail. This is the DLSS-shaped outcome,
  built entirely from Phase 1's machinery (no ML, no vendor SDK, runs in GL 3.3).

---

## Goals / Non-goals

**Goals**
- Offscreen HDR render + ping-pong history + present/tonemap pass.
- Frame-varying seed and Halton sub-pixel jitter.
- Camera-driven reprojection (no motion vectors) with `t`-packed-in-alpha world-pos
  reconstruction (no MRT).
- Disocclusion rejection + clamped EMA accumulation.
- Reduced-resolution internal render with temporal upsample to full res.
- A UI/runtime toggle for: temporal on/off, internal resolution scale, max history length.

**Non-goals (deferred)**
- True DLSS/XeSS/FSR2 vendor SDKs (require Vulkan/DX + RTX/SDK; out of scope for GL 3.3).
- Self-built ML inference (no compute shaders in GL 3.3).
- Moving light / day-night (the foundation supports it — reprojection handles it — but it's
  not a target now; if added, the still-camera Phase-0 optimization no longer fully applies).
- Variance-guided / spatial denoisers (SVGF-style) layered on top of accumulation.
- World-space irradiance cache (the alternative amortization strategy; separate spec if pursued).

---

## Risks / hard parts

- **Resolve-pass tuning (the main time sink).** The disocclusion reject threshold trades
  ghosting against re-noising at edges; expect iteration. Mitigated by Phase 0 shipping
  first without it.
- **HDR precision & tonemap ordering.** Accumulation must happen in linear HDR; tone mapping
  must move to the present pass. Getting this wrong gives muddy or banded output (the trace
  shader currently tonemaps inline at `:282-286`).
- **History validity on big camera jumps** (teleport/reset view): detect large camera deltas
  and hard-reset history to avoid smearing.
- **raylib FBO format support:** confirm RGBA16F float render textures are available on the
  target GL 3.3 context; fall back to RGBA8 + careful ranges only if forced.

## Open questions (resolve during planning)

- Internal resolution scale default for Phase 2 (50% vs 67%) and whether it's dynamic.
- Max history length / blend factor defaults, and whether to expose hysteresis on
  camera-change detection so micro-jitter doesn't reset accumulation every frame.
- Whether secondary-ray sample reduction (shadows/GI to ~1 spp) lands in Phase 1 or is a
  follow-on once accumulation is stable.
