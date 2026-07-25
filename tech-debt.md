# Tech Debt

Known debt with evidence, so nobody has to re-derive it. Each item records what
was **ruled out** as well as what's outstanding — several of these look worse
than they are, and one looks smaller than it is.

Nothing here is scheduled. Last audited 2026-07-25.

---

## 1. `Matrix4x4` duplicates `mat4` — with conversion shims between them

**The one real duplicate in the math layer.**

`libs/MatterSurfaceLib/src/tlas_manager.cpp` carries a full 4×4 matrix suite on its
own `Matrix4x4` type, sitting beside `mat4` (from `libs/SpatialQueryLib/include/tri.h`)
in the same file, with explicit converters between the two:

```cpp
mat4      TLASManager::convert_matrix(const Matrix4x4& legacy_matrix);  // :15
Matrix4x4 TLASManager::convert_matrix_back(const mat4& new_matrix);     // :23
```

Both are row-major `float[16]`. Same layout, same convention, same layer,
adjacent classes. The parameter name `legacy_matrix` suggests this was known.

**Where it came from.** The nine functions are a C++ port of the matrix API that
lived in SpatialQueryLib's C BVH — the implementation deleted in `c3f0577a` as
having no consumers. It had no consumers; its *matrix half* had already been
copied here and is load-bearing.

| Deleted `libs/SpatialQueryLib/include/bvh.h` (see `c3f0577a^`) | Live in `tlas_manager.cpp` |
|---|---|
| `matrix_identity`, `matrix_multiply`, `matrix_inverse`, `matrix_translation`, `matrix_scale`, `matrix_rotation_x/y/z/axis` | all nine, identical names and `const Matrix4x4*` signatures |
| `matrix_transform_point`, `matrix_transform_vector` | not carried over |

**Still load-bearing** — not vestigial:
- `libs/MatterSurfaceLib/include/tlas_manager.hpp:21` (definition; 14 uses in that header)
- `MatterEngine3/src/tileset_torus_bvh.cpp:162,169,299` — builds transforms from poses
- `MatterEngine3/src/render/tileset_bake_vk.cpp:420` — row-major → `VkTransformMatrixKHR`
- `MatterEngine3/tests/part_asset_v2_tests.cpp:96-99`

**Fix.** Merge onto `mat4`: delete the nine functions and both shims. Self-contained,
mechanical, one file. Same shape as the `spatial_hash` consolidation (`a349f723`).

---

## 2. Four matrix inverses, four different failure modes

This is a **correctness** item, not tidiness. The duplication is secondary; the
divergent handling of singular input is the actual hazard.

| Implementation | Type | On singular input |
|---|---|---|
| `MatterEngine3/src/render/matrix_math.cpp:120` `mat4_inverse` | `Mat4f` | returns `false` — caller decides |
| `libs/SpatialQueryLib/include/tri.h:57` `mat4::Inverted` | `mat4` | returns **identity** |
| `MatterEngine3/src/csg_lowering.cpp:24` `mat_invert` | raylib `Matrix` | returns **zero matrix** (guard at `:37`) |
| `libs/MatterSurfaceLib/src/tlas_manager.cpp:49` `matrix_inverse` | `Matrix4x4` | (fourth implementation) |
| `third_party/raylib/src/raymath.h:1538` `MatrixInvert` | raylib `Matrix` | **no guard** — computes `1.0f/det`, yields inf/NaN |

A degenerate transform silently becomes identity on one path, a zero matrix on
another, and NaN on a third.

**Ruled out:** `csg_lowering::mat_invert` looks like a gratuitous reimplementation
of raymath's `MatrixInvert` for the same type — its own comment even says it
"matches raymath's MatrixInvert semantics". It is **not** gratuitous: raymath has
no zero-determinant guard, and `mat_invert` adds one. The reimplementation exists
*for* the guard. Don't delete it without replacing the guard.

**Fix.** Pick one singular-matrix policy and apply it across all four.

---

## 3. Three matrix multiplies, because there are three matrix types

All inside MatterEngine3:

- `MatrixMultiply` (raymath) — `src/dsl_state.cpp:31-40,76`, the DSL transform stack
- `mat4_mul` (on `Mat4f`) — `src/matter_engine.cpp:4513`, `src/provider/resolvers.cpp:107`
- `mul16` (raw `float[16]`) — `src/mat_math.h:27`, used by `src/part_flatten.cpp:336,421`

Not duplicates of each other in the strict sense — different types — but they
exist *because* of item 4. Folding them is downstream of that decision.

---

## 4. Three vector/matrix type families — **deliberate; do not unify**

Recorded so this isn't re-audited as if it were debt.

| Family | Home | Reach | Role |
|---|---|---|---|
| `float3`/`float4`/`mat4` | `libs/SpatialQueryLib/include/{precomp,tri}.h` | **61 files** | SIMD-aligned, shaped for BVH build |
| `matter::Float3`/`Float4`/`Quaternion`/`Mat4f` | `MatterEngine3/include/matter/math_types.h` | **24 files** | the exported engine API boundary |
| raylib `Vector3`/`Matrix`/`Quaternion` | vendored | DSL/CSG layer | where the code is raylib-facing |

**The reason not to unify:** there is no conversion tax to recover. A search for
cross-family converters found exactly one — `row_major_to_matrix`
(`MatterEngine3/src/render/raster_mesh.cpp:158`, declared `.h:40`). The families
are cleanly separated by layer, not tangled. Unifying would touch 85+ files to
remove a cost nobody is paying.

Revisit only if conversions start appearing at layer boundaries.

---

## 5. Verified **not** debt — don't re-audit these

| Thing | Why it's legitimately separate |
|---|---|
| `libs/MatterSurfaceLib/src/mesh_simplifier.cpp:80` `V3` | **double** precision — Garland-Heckbert quadrics need it. Comment: "avoids raymath coupling" |
| `libs/ParticleFlowLib/include/particle_flow.h:14` `pf::V3` | deterministic sim, header-only public API |
| `MatterEngine3/shared-lib/vecmath.js` | QuickJS scripting runtime — different language |
| `MatterEngine3/src/render/vk_gi_math.*` | BRDF/GGX sampling, builds on `matter::Float3` |
| `MatterEngine3/src/render/gpu_matrix_pack.h:7` `GpuMat4` | GPU packing layout |
| `MatterEngine3/src/mat_math.h:46` `NormalMat` | 3×3 inverse-transpose for normals |
| `terrain_mesher.cpp:12`, `mesh_charting.cpp:10`, `selection_outline.cpp:11` | POD structs, **zero** operations defined |

Minor and real: `MatterEditor/viewport_pick.cpp:17` defines `Vec3` plus
`add`/`cross`/`normalize`/`scale`/`sub` with no precision or determinism reason.

---

## 6. GL BLAS/TLAS upload machinery — delete with the GL path

The GL ray-tracing path is being replaced by Vulkan. When it goes, delete rather
than refactor:

- `textures_dirty_`, `shader_values_dirty_`, per-entry `gpu_dirty`, the two
  `Texture2D` members, `ensure_gpu_textures_ready()`, `bind_to_shader()`
  (BLAS and TLAS managers)
- `libs/MatterSurfaceLib/main.cpp:1150,1151,1496` also calls `bind_to_shader`

`content_revision()` (landed `b11d36fc`) is the backend-agnostic replacement: a
Vulkan uploader holds its own `seen_content_revision_` and skips when equal.
O(1), and per-consumer — a shared dirty *flag* is cleared by whoever services it
first, silently starving a second uploader.

**Two GL-specific facts to carry forward**, currently recorded only as comments
in code slated for deletion:
1. `bind_to_shader` stages textures **every frame regardless of dirty state** —
   raylib's batch resets `activeTextureId` after each draw.
2. `TLASManager::mark_dirty` resets `cached_shader_id_ = 0` — GL reuses program
   ids after a shader is deleted, so a cached uniform location can silently
   belong to a stale program.

---

## 7. Smaller open items

- **`Prototypes/GPURayTraceExample` duplicates.** 11 file pairs against
  MatterSurfaceLib (`blas_manager`, `tlas_manager`, `bvh_visualizer`, …), plus
  `precomp.h` and `bvh.{cpp,h}` against SpatialQueryLib. `precomp.h` is still
  **byte-identical** to SpatialQueryLib's. No maintenance cost while parked and
  out of `build-all.sh`; the whole class disappears if that prototype is ever
  deleted. It is the last duplicate cluster left in the tree.

- **`libs/MatterSurfaceLib/Makefile` hardcodes `x86_64-w64-mingw32-g++-posix`**, which
  is not installed under MSYS2 UCRT64 (plain `x86_64-w64-mingw32-g++` is;
  MatterEditor uses `/ucrt64/bin/g++`). `make -C libs/MatterSurfaceLib` fails at the
  compiler, not at any source. Pre-existing.

- **ASan is unavailable** in MSYS2 UCRT64 (`cannot find -lasan`).
  `SpatialQueryLib` and `MemoryLib` `test` targets are sanitizer-based, so on
  Windows they are quietly weaker than the Makefile implies.

- **`SpatialQueryLib`'s name under-describes it.** It owns `float3`/`float4`/
  `Tri`/`mat4` and the BVH — a geometry + acceleration foundation, not a query
  library. Renaming is churn across ~20 include paths; deliberately deferred.

- **`origin/main` is behind local `main`** by the full consolidation series plus
  ~65 earlier commits. Independent of the work above.
