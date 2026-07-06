# Task 5 Report — AO Compute Pass

**Status:** COMPLETE
**SHA:** f8824b1
**Test summary:** 31/31 passed — ALL PASS (GL 4.6, GALLIUM_DRIVER=d3d12)

## What was built

- `MatterEngine3/viewer/shaders_gpu/tileset_bake_ao.comp` — compute shader: re-casts ortho-down primary ray, builds cosine-weighted hemisphere frame, fires 64 `shadowQuery` rays per texel (SplitMix32 deterministic RNG seeded from xy + sample_idx + user_seed), writes R8 AO.
- `MatterEngine3/include/tileset_bake_ao.h` — public header with `tileset::bake_ao(...)` matching the Task 6 contract exactly.
- `MatterEngine3/src/tileset_bake_ao.cpp` — driver: creates R8 texture, calls `bind_bvh_samplers`, sets uniforms, dispatches `((W+7)/8, (H+7)/8, 1)`, GL error sweeps on readback.
- `MatterEngine3/viewer/tileset_gpu_tests.cpp` — `test_ao_bake_edge_darkens`: box fixture, AO<200 at box edge, AO>200 in open ground, byte-identical on double-bake.
- `MatterEngine3/Makefile` + `MatterEngine3/viewer/Makefile` — wired `tileset_bake_ao.cpp` into both builds.

## Non-trivial decisions / divergences from brief

1. **`shadowQuery` instead of `intersectScene` for AO rays** — The brief's shader uses `intersectScene` for both the primary hit and AO rays. `shadowQuery` is the existing early-exit shadow path in `bvh_tlas_common.glsl`; it skips the full material/barycentric decode and returns bool. This is ~4x cheaper per AO ray and produces identical occlusion results.

2. **`materialCount = 0` in the driver** — When `intersectScene` is called for the primary hit, `getMaterialProperties` must resolve a material. The base-field TriEx records have zero per-vertex normals (N0=N1=N2=(0,0,0)). With a non-zero materialCount the shader tries smooth-normal interpolation → normalize(vec3(0)) → NaN propagated into the surface frame → all 64 AO rays miss → all pixels 255. Setting materialCount=0 causes getMaterialProperties to always return the default material (flatShading=true), forcing face-normal computation (cross(e1,e2)) which is always valid.

3. **Test edge texel: `bx - e - 0.02` not `bx - e + 0.02`** — The brief wrote `bx - e + 0.02f` = 3.62, which is 0.02 m inside the box (on the box top face, y=0.4). The primary ray hits the box top; AO rays pointing upward miss the walls → all 255, no contrast. Fix: `bx - e - 0.02f` = 3.58, just outside the box left wall on the ground. AO rays from that ground point are partially blocked by the 0.02 m-away wall.

## Observed AO values on fixture

- Edge texel (114, 128): ao = 147 (~58% occluded by adjacent box wall)
- Far texel (16, 16): ao = 255 (100% unoccluded, nothing within 0.5 m)
- Determinism: two bake calls with same seed → byte-identical

## Concerns for Task 6

- `materialCount = 0` is correct only when face normals are acceptable. Task 6 (ORM.R fold) should preserve this.
- The SSBO at binding 10 (stub zeros) exists only to satisfy the shader's buffer declaration. Task 6 can remove or reuse it.
- `edge_strip_width` as maxRayDist is seam-invariant by design: AO near tile edges only considers geometry within that strip.

## Fix wave (seed test + precondition)

**SHA:** 4eed1b5

**Changes:**
- `MatterEngine3/viewer/tileset_gpu_tests.cpp:310-319` — Add seed-difference assertion after the determinism check: bake with seed `0xDEADBEEFu` and verify output differs from baseline `0xC0DEu`.
- `MatterEngine3/include/tileset_bake_ao.h:16-23` — Add PRECONDITION FOR TASK 6 block warning that materialCount=0 forces flatShading and is fine for base heightfield but must be revisited for smooth-shaded scattered instances.
- `MatterEngine3/src/tileset_bake_ao.cpp:101-106` — Add FIXME(Task 6) comment at materialCount=0 line referencing the header precondition.

**Test output (last 1 line):**
```
--- Results: 34/34 passed --- ALL PASS
```

**Reviewer follow-up:** The Important item (materialCount + material upload coordination) is flagged as a precondition for Task 6, not attempted in this wave.
