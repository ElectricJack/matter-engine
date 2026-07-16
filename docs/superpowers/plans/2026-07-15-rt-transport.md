# RT Material Transport Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the RT shading path consume the transport data the lighting sculpture garden already authors: colored emission, dielectric transmission, thin-walled/wax scattering, and a second diffuse GI bounce.

**Architecture:** Extend the existing rt_lighting raygen, composite fragment shader, and shadow any-hit in place — no new pipelines, passes, or denoiser lanes. Transmission gets one new rgba16f image (`raw_transmission_`) that bypasses the temporal/atrous chain and feeds composite directly. Spec: `docs/superpowers/specs/2026-07-15-rt-transport-design.md`.

**Tech Stack:** Vulkan ray tracing (GLSL 460, GL_EXT_ray_tracing), C++17 renderer (`vk_scene_renderer.cpp`), C material registry (MatterSurfaceLib).

## Global Constraints

- **NO verification gates.** Do not run test suites. Do not attempt to compile shaders (there is no glslc in WSL; Jack's MSYS2 Windows build regenerates `embedded_spirv.h` at the end and he does all runtime testing). Shader changes are static-review-only.
- The only permitted check per task is a free `-fsyntax-only` compile of the touched C++/C translation units, using the exact commands given in each task. If the syntax check fails, fix and re-run; do not expand scope.
- MatterSurfaceLib is normally read-only, but Task 1's `material_registry.c` packer change is explicitly in-scope (approved in the spec; the garden presets commit already extended this file).
- One commit per task, in order 1 → 4. Commit exactly the files listed. Never push. Never use `--no-verify` or `--amend`.
- Match surrounding code style: 4-space indent, ~80-column lines, no trailing whitespace, GLSL constants/layouts formatted like neighboring declarations.
- Do not add UI tunables, push-constant fields, comments about "added for task N", or any behavior beyond what each task specifies.
- All `vk_scene_renderer.cpp` line numbers below were captured before any task ran; later tasks must locate code by the quoted anchor text, since earlier tasks shift lines.

**C++ syntax check commands** (referenced by tasks as CHECK-RENDERER and CHECK-REGISTRY):

CHECK-RENDERER — run from `MatterEngine3/`:

```bash
g++ -fsyntax-only -std=c++17 -Wall -Wno-missing-braces -Wno-unused-variable \
    -DPLATFORM_DESKTOP -DGRAPHICS_API_OPENGL_43 -DMATTER_HAVE_SCRIPT_HOST \
    -DTILESET_GTEX_USE_RAYLIB_STB \
    -Iinclude -Isrc -Isrc/render -Isrc/provider \
    -I../MatterSurfaceLib/include -I../MatterSurfaceLib/src \
    -I../Libraries/quickjs-ng -I../Libraries/raylib/src \
    -I../Libraries/box3d/include -I../ParticleFlowLib/include \
    -I../Libraries/Vulkan-Headers/include \
    src/render/vk_scene_renderer.cpp
```

CHECK-REGISTRY — run from `MatterSurfaceLib/`:

```bash
gcc -fsyntax-only -Wall -Iinclude src/material_registry.c
```

Expected for both: no output, exit 0.

---

### Task 1: Colored emission

Emissive materials currently glow with their albedo color because both shading
paths ignore the authored `emission_strength.rgb`. Fix the packer so legacy
emissive materials (black emission color) get their albedo copied into
`emission_strength.rgb`, then read `emission_strength.rgb` unconditionally in
both shaders. This task also lands the composite-pass plumbing (identity
texture + material buffer bindings) that Tasks 2 and 3 build on.

**Files:**
- Modify: `MatterSurfaceLib/src/material_registry.c` (packer, in `MaterialRegistryPackRtForGPU`)
- Modify: `MatterEngine3/shaders_vk/rt_lighting.rgen` (split `hit_radiance`, lines 152-162)
- Modify: `MatterEngine3/shaders_vk/composite.frag` (new bindings 6/7, emission color)
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp` (composite set layout, pool sizes, `update_composite_descriptor`)

**Interfaces:**
- Consumes: `MaterialGpuRecord`/`RtMaterialGpu` layout (9 vec4/uvec4 fields, already defined); `frame.materials` storage buffer (always allocated per frame slot; already bound to raster set 1 binding 5); `material_instance_` R32G32_UINT G-buffer image where `.r` = material index.
- Produces (later tasks rely on these exact names):
  - GLSL `vec3 hit_emission(RtSurface surface, out vec3 base)` and `vec3 hit_radiance(RtSurface surface, out vec3 base)` in rt_lighting.rgen (Task 4 uses both).
  - composite.frag binding 6 `usampler2D identity_texture`, binding 7 std430 readonly buffer `RtMaterialTable { RtMaterialGpu rt_materials[]; }` with a local `struct RtMaterialGpu` (Tasks 2 and 3 read these).
  - Composite descriptor-set layout: bindings 0-6 = COMBINED_IMAGE_SAMPLER, binding 7 = STORAGE_BUFFER (Task 2 appends binding 8).

- [ ] **Step 1: Normalize legacy emission color at pack time**

In `MatterSurfaceLib/src/material_registry.c`, inside
`MaterialRegistryPackRtForGPU`, the emission block currently reads:

```c
        r->emission_strength[0] = m->emissionColor[0];
        r->emission_strength[1] = m->emissionColor[1];
        r->emission_strength[2] = m->emissionColor[2];
        r->emission_strength[3] = m->emission;
```

Replace with:

```c
        r->emission_strength[0] = m->emissionColor[0];
        r->emission_strength[1] = m->emissionColor[1];
        r->emission_strength[2] = m->emissionColor[2];
        r->emission_strength[3] = m->emission;
        /* Legacy emissive materials author emission > 0 with a black
           emission color; normalize to albedo so shaders can read
           emission_strength rgb unconditionally. */
        if (m->emission > 0.0f && m->emissionColor[0] <= 0.0f &&
            m->emissionColor[1] <= 0.0f && m->emissionColor[2] <= 0.0f) {
            r->emission_strength[0] = m->albedo[0];
            r->emission_strength[1] = m->albedo[1];
            r->emission_strength[2] = m->albedo[2];
        }
```

- [ ] **Step 2: Split `hit_radiance` and use emission color in rt_lighting.rgen**

In `MatterEngine3/shaders_vk/rt_lighting.rgen`, replace the entire
`hit_radiance` function (currently lines 152-162):

```glsl
vec3 hit_radiance(RtSurface surface, out vec3 base) {
    base = vec3(0.0);
    if ((surface.flags & RT_SURFACE_VALID) == 0u) return vec3(0.0);
    if (surface.material_index >= rt_materials.length()) return vec3(0.0);
    RtMaterialGpu material = rt_materials[surface.material_index];
    base = mix(material.base_roughness.rgb, surface.tint.rgb,
               clamp(surface.tint.a, 0.0, 1.0));
    vec3 emission = base * max(material.emission_strength.w, 0.0) *
                    constants.emission_multiplier;
    return emission + sky_irradiance(surface.normal) * base;
}
```

with:

```glsl
vec3 hit_emission(RtSurface surface, out vec3 base) {
    base = vec3(0.0);
    if ((surface.flags & RT_SURFACE_VALID) == 0u) return vec3(0.0);
    if (surface.material_index >= rt_materials.length()) return vec3(0.0);
    RtMaterialGpu material = rt_materials[surface.material_index];
    float tint_blend = clamp(surface.tint.a, 0.0, 1.0);
    base = mix(material.base_roughness.rgb, surface.tint.rgb, tint_blend);
    vec3 emission_color = mix(material.emission_strength.rgb,
                              surface.tint.rgb, tint_blend);
    return emission_color * max(material.emission_strength.w, 0.0) *
           constants.emission_multiplier;
}

vec3 hit_radiance(RtSurface surface, out vec3 base) {
    vec3 emission = hit_emission(surface, base);
    return emission + sky_irradiance(surface.normal) * base;
}
```

No other rt_lighting.rgen change in this task. All existing `hit_radiance`
call sites keep working unchanged.

- [ ] **Step 3: Composite shader — identity + material bindings, colored emission**

Replace the full contents of `MatterEngine3/shaders_vk/composite.frag` with:

```glsl
#version 460

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_hdr;

layout(set = 0, binding = 0) uniform sampler2D albedo_texture;
layout(set = 0, binding = 1) uniform sampler2D normal_texture;
layout(set = 0, binding = 2) uniform sampler2D orm_texture;
layout(set = 0, binding = 3) uniform sampler2D visibility_texture;
layout(set = 0, binding = 4) uniform sampler2D raw_diffuse_texture;
layout(set = 0, binding = 5) uniform sampler2D specular_texture;
layout(set = 0, binding = 6) uniform usampler2D identity_texture;

// Mirrors MaterialGpuRecord / rt_surface_common.glsl RtMaterialGpu; declared
// locally because rt_surface_common.glsl claims set 0 bindings 3-5.
struct RtMaterialGpu {
    vec4 base_roughness;
    vec4 metal_opacity_spec_coat;
    vec4 specular_tint_coat_roughness;
    vec4 emission_strength;
    vec4 transmission;
    vec4 absorption_pad;
    vec4 scattering;
    vec4 scattering_shape;
    uvec4 flags_misc;
};

layout(set = 0, binding = 7, std430) readonly buffer RtMaterialTable {
    RtMaterialGpu rt_materials[];
};

layout(push_constant) uniform SceneLighting {
    vec3 sun_direction;
    float sun_intensity;
    vec3 sun_color;
    float diffuse_rt_multiplier;
    vec3 sky_color;
    float emission_multiplier;
    float debug_view;
    float pad0;
    float pad1;
    float pad2;
} lighting;

void main() {
    vec4 albedo = texture(albedo_texture, in_uv);
    vec4 normal_payload = texture(normal_texture, in_uv);
    vec3 normal_sample = normal_payload.xyz;
    float normal_length_squared = dot(normal_sample, normal_sample);
    vec3 normal = normal_length_squared > 1e-20
                    ? normal_sample * inversesqrt(normal_length_squared)
                    : vec3(0.0);
    vec4 orm = texture(orm_texture, in_uv);
    vec3 to_sun = normalize(-lighting.sun_direction);
    float direct = max(dot(normal, to_sun), 0.0);
    float roughness = orm.x;
    float metallic = orm.y;
    float ao = orm.z;
    vec3 diffuse = albedo.rgb * (1.0 - metallic);
    vec3 ambient = diffuse * lighting.sky_color * ao;
    vec3 visibility = texture(visibility_texture, in_uv).rgb;
    if (lighting.debug_view > 0.5) {
        out_hdr = vec4(visibility, 1.0);
        return;
    }
    vec3 sun = diffuse * lighting.sun_color * direct * lighting.sun_intensity *
               visibility;
    float encoded_emission = normal_payload.w;
    float emission_strength =
        !isnan(encoded_emission) && !isinf(encoded_emission) &&
        encoded_emission > 0.0
            ? exp2(min(encoded_emission, 15.875)) - 1.0
            : 0.0;
    uint material_index = texelFetch(identity_texture,
                                     ivec2(gl_FragCoord.xy), 0).r;
    vec3 emission_color = material_index < rt_materials.length()
        ? rt_materials[material_index].emission_strength.rgb
        : albedo.rgb;
    vec3 emission = emission_color * emission_strength *
                    lighting.emission_multiplier;
    vec3 raw_diffuse = texture(raw_diffuse_texture, in_uv).rgb *
                       lighting.diffuse_rt_multiplier;
    vec3 specular = texture(specular_texture, in_uv).rgb;
    vec3 linear_hdr = ambient + sun * mix(1.0, 0.65, roughness) + emission +
                      raw_diffuse + specular;
    out_hdr = vec4(linear_hdr, 1.0);
}
```

(Known, accepted limitation from the spec: composite emission uses the
material record's color, not the per-instance tint blend; the G-buffer does
not carry the tint. The RT paths do blend tint via `hit_emission`.)

- [ ] **Step 4: Grow the composite descriptor-set layout to 8 bindings**

In `MatterEngine3/src/render/vk_scene_renderer.cpp`, find (near line 1459):

```cpp
    std::array<VkDescriptorSetLayoutBinding, 6> sampled_bindings{};
    for (uint32_t i = 0; i < sampled_bindings.size(); ++i) {
        sampled_bindings[i] = descriptor_binding(
            i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_SHADER_STAGE_FRAGMENT_BIT);
    }
```

Replace with:

```cpp
    std::array<VkDescriptorSetLayoutBinding, 8> sampled_bindings{};
    for (uint32_t i = 0; i < 7; ++i) {
        sampled_bindings[i] = descriptor_binding(
            i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_SHADER_STAGE_FRAGMENT_BIT);
    }
    sampled_bindings[7] = descriptor_binding(
        7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT);
```

- [ ] **Step 5: Bump the frame descriptor pool**

Same file, near line 1775, find:

```cpp
    const VkDescriptorPoolSize pool_sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, frame_slot_count},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, frame_slot_count * 12},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         frame_slot_count * 75},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, frame_slot_count * 22}};
```

Change `* 12` to `* 13` (composite material buffer) and `* 75` to `* 76`
(composite identity sampler). Leave the other two counts alone.

- [ ] **Step 6: Write the two new composite descriptors**

Same file, replace the body of `update_composite_descriptor` from
`matter::VkImageResource* sampled[] = ...` through the
`vkUpdateDescriptorSets` call (near lines 1963-1979) with:

```cpp
    matter::VkImageResource* sampled[] = {&albedo_, &normal_, &orm_,
                                          &visibility_, diffuse, specular,
                                          &material_instance_};
    VkDescriptorImageInfo image_infos[7]{};
    VkWriteDescriptorSet writes[8]{};
    for (uint32_t i = 0; i < 7; ++i) {
        image_infos[i].sampler = composite_sampler_;
        image_infos[i].imageView = sampled[i]->view;
        image_infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = frame.composite_descriptor_set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType =
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &image_infos[i];
    }
    VkDescriptorBufferInfo material_info{frame.materials.buffer, 0,
                                         frame.materials.size};
    writes[7].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[7].dstSet = frame.composite_descriptor_set;
    writes[7].dstBinding = 7;
    writes[7].descriptorCount = 1;
    writes[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[7].pBufferInfo = &material_info;
    vkUpdateDescriptorSets(vulkan_->device(), 8, writes, 0, nullptr);
```

(`frame.materials` is created for every frame slot before any composite
descriptor update runs — same guarantee the raster set relies on — and
`material_instance_` is a G-buffer attachment that is transitioned to
SHADER_READ_ONLY before composite along with albedo/normal/orm. The
usampler2D + composite_sampler_ pairing follows the existing precedent of RT
set binding 11.)

- [ ] **Step 7: Syntax checks**

Run CHECK-RENDERER and CHECK-REGISTRY (commands in Global Constraints).
Expected: both exit 0 with no output.

- [ ] **Step 8: Commit**

```bash
git add MatterSurfaceLib/src/material_registry.c \
        MatterEngine3/shaders_vk/rt_lighting.rgen \
        MatterEngine3/shaders_vk/composite.frag \
        MatterEngine3/src/render/vk_scene_renderer.cpp
git commit -m "feat(vulkan): consume authored emission color in RT and composite"
```

---

### Task 2: Dielectric transmission

Add a deterministic refraction walk for `transmission.x > 0` materials
(greenGlass, water, glassSmoke, legacy glass) in rt_lighting.rgen, writing to
a new `raw_transmission_` rgba16f image that bypasses the denoiser and blends
in composite. See spec §3 for the model.

**Files:**
- Modify: `MatterEngine3/src/render/vk_scene_renderer.h` (one member after `raw_specular_aux_`, line ~831)
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp` (RT set layout/pool/writes, image lifecycle, composite layout/descriptor, RasterRecord)
- Modify: `MatterEngine3/shaders_vk/rt_lighting.rgen` (binding 14, transmission walk)
- Modify: `MatterEngine3/shaders_vk/composite.frag` (binding 8, blend)

**Interfaces:**
- Consumes: Task 1's composite bindings 6/7 and layout (bindings 0-6 samplers, 7 storage buffer); existing `hit` payload, `hit_radiance(RtSurface, out vec3)`, `environment(vec3)`, `invalid_rt_surface()`, `RT_SURFACE_VALID`/`RT_SURFACE_FRONT_FACE`; `transition_for_use` / `clear_color_image_for_use` helpers.
- Produces: member `matter::VkImageResource raw_transmission_` (rgba16f, raw_diffuse extent; rgb = transmitted radiance × (1−F) × Beer-Lambert, a = coverage weight); RT set binding 14 (`raw_transmission_image`, storage image, raygen); composite binding 8 (`transmission_texture`, sampler). Composite layout final shape: 0-6 samplers, 7 storage buffer, 8 sampler — Task 3 does not change it further.

- [ ] **Step 1: Header member**

In `MatterEngine3/src/render/vk_scene_renderer.h`, after
`matter::VkImageResource raw_specular_aux_;` add:

```cpp
    matter::VkImageResource raw_transmission_;
```

- [ ] **Step 2: RT descriptor-set layout binding 14 + pools**

In `vk_scene_renderer.cpp`, in `create_ray_tracing_pipeline` (near line
1171), the last entry of the `bindings[]` array is:

```cpp
        descriptor_binding(13, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR)};
```

Replace with:

```cpp
        descriptor_binding(13, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR),
        descriptor_binding(14, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR)};
```

and change `set_info.bindingCount = 14;` to `set_info.bindingCount = 15;`.

Near line 1900, in the RT pool sizes, change
`{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, frame_slot_count * 4}` to
`{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, frame_slot_count * 5}`.

Near line 1778 (already bumped by Task 1), change the frame pool's
`VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER` count from
`frame_slot_count * 76` to `frame_slot_count * 77` (composite transmission
sampler).

- [ ] **Step 3: Image lifecycle**

All in `vk_scene_renderer.cpp`:

a. In `destroy_pipeline` (near line 848), after
`raw_specular_aux_.reset();` add `raw_transmission_.reset();`.

b. In `ensure_raster_targets`: after the local declaration
`matter::VkImageResource raw_specular_aux;` (near line 5213) add
`matter::VkImageResource raw_transmission;`. In the big `create_image` `||`
chain, the raw_specular_aux clause currently ends the chain:

```cpp
        !matter::create_image(
            *vulkan_, VK_IMAGE_TYPE_2D, VK_FORMAT_R16G16_SFLOAT, raw_extent,
            visibility_usage, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, raw_specular_aux, error)) {
        return false;
    }
```

Replace with:

```cpp
        !matter::create_image(
            *vulkan_, VK_IMAGE_TYPE_2D, VK_FORMAT_R16G16_SFLOAT, raw_extent,
            visibility_usage, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, raw_specular_aux, error) ||
        !matter::create_image(
            *vulkan_, VK_IMAGE_TYPE_2D,
            VK_FORMAT_R16G16B16A16_SFLOAT, raw_extent, visibility_usage,
            VK_IMAGE_ASPECT_COLOR_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            raw_transmission, error)) {
        return false;
    }
```

c. After `raw_specular_aux_ = std::move(raw_specular_aux);` (near line
5341) add `raw_transmission_ = std::move(raw_transmission);`.

d. In the early-return validity check (near line 5192), after
`raw_specular_aux_.image != VK_NULL_HANDLE &&` add
`raw_transmission_.image != VK_NULL_HANDLE &&`.

- [ ] **Step 4: Clear, bind, transition, retain in the RT dispatch path**

All in `vk_scene_renderer.cpp`, function `record_ray_trace_dispatch`:

a. Near line 4653, change the clear loop to include the new image:

```cpp
    for (auto* specular_image : {&raw_specular_, &raw_specular_aux_,
                                 &raw_transmission_}) {
```

b. Near line 4680, after `raw_specular_aux_info` add:

```cpp
    VkDescriptorImageInfo raw_transmission_info{VK_NULL_HANDLE,
                                                raw_transmission_.view,
                                                VK_IMAGE_LAYOUT_GENERAL};
```

c. Change `VkWriteDescriptorSet writes[14]{};` to
`VkWriteDescriptorSet writes[15]{};`, then change the trailing write loop
(near line 4733) to:

```cpp
    VkDescriptorImageInfo* extra_infos[] = {
        &identity_info, &raw_specular_info, &raw_specular_aux_info,
        &raw_transmission_info};
    for (uint32_t i = 11; i < 15; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = rt_descriptor_sets_[frame.frame_slot];
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = i == 11
            ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
            : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[i].pImageInfo = extra_infos[i - 11];
    }
    vkUpdateDescriptorSets(vulkan_->device(), 15, writes, 0, nullptr);
```

d. After the `composite_specular` transition (the `transition_for_use(...)`
call near line 4876-4880), add — unconditionally, since the transmission
signal bypasses the denoiser and composite always samples it:

```cpp
    transition_for_use(frame.command_buffer, raw_transmission_,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT);
```

e. In the `retained` lifetime vector (near line 4881), after
`raw_specular_aux_.lifetime,` add `raw_transmission_.lifetime,`.

- [ ] **Step 5: Fallback clears when RT is off**

a. In `record_ray_traced_shadows`'s `clear_raw_diffuse` lambda (near line
4226), change the loop to:

```cpp
        for (auto* image : {&raw_diffuse_, &raw_specular_,
                            &raw_specular_aux_, &raw_transmission_}) {
```

b. In `struct RasterRecord` (near line 270), after
`matter::VkImageResource* raw_specular;` add
`matter::VkImageResource* raw_transmission;`.

c. In the no-renderer fallback clear (near line 429), change:

```cpp
        for (auto* signal : {record.raw_diffuse, record.raw_specular})
```

to:

```cpp
        for (auto* signal : {record.raw_diffuse, record.raw_specular,
                             record.raw_transmission})
```

d. Find every aggregate construction of `RasterRecord` (grep
`RasterRecord record` — the main one is in `prepare_frame` near line 5017,
initializing `&raw_diffuse_,` then `&raw_specular_,`) and insert
`&raw_transmission_,` immediately after `&raw_specular_,` so positional
initialization matches the new field order.

e. In `prepare_frame`'s `attachments` lifetime vector (near line 4967),
after `raw_specular_.lifetime, raw_specular_aux_.lifetime,` add
`raw_transmission_.lifetime,`.

A cleared coverage weight of 0 makes the composite blend an exact no-op, so
raster-only frames are unaffected.

- [ ] **Step 6: rt_lighting.rgen — binding 14, early-out store, transmission walk**

a. After the `raw_specular_aux_image` binding declaration (line 15) add:

```glsl
layout(set = 0, binding = 14, rgba16f) uniform image2D raw_transmission_image;
```

b. In the early-out block (lines 180-185), after
`imageStore(raw_specular_aux_image, pixel, vec4(0.0));` add:

```glsl
        imageStore(raw_transmission_image, pixel, vec4(0.0));
```

c. At the end of `main()`, immediately after the two existing `imageStore`
calls for specular (lines 309-310), append the transmission lane:

```glsl
    vec3 transmitted = vec3(0.0);
    float transmission_weight = 0.0;
    if (identity.x < rt_materials.length()) {
        RtMaterialGpu trans_material = rt_materials[identity.x];
        transmission_weight = clamp(trans_material.transmission.x, 0.0, 1.0);
        if (transmission_weight > 0.0) {
            float ior = max(trans_material.transmission.y, 1.0001);
            vec4 trans_near_h = constants.clip_to_world *
                vec4(source_uv.x * 2.0 - 1.0, 1.0 - source_uv.y * 2.0,
                     0.0, 1.0);
            vec3 trans_near_world = trans_near_h.xyz / trans_near_h.w;
            vec3 trans_view = normalize(trans_near_world - world);
            float f0 = pow((ior - 1.0) / (ior + 1.0), 2.0);
            float fresnel = f0 + (1.0 - f0) *
                pow(1.0 - clamp(dot(trans_view, normal), 0.0, 1.0), 5.0);
            vec3 refracted = refract(-trans_view, normal, 1.0 / ior);
            if (dot(refracted, refracted) < 1e-8) {
                // Entry TIR (only reachable for authored ior < 1); the
                // specular lane carries the reflected energy.
                transmission_weight = 0.0;
            } else {
                vec3 walk_origin = world - normal * constants.bias;
                vec3 walk_dir = normalize(refracted);
                float path_length = 0.0;
                bool shaded = false;
                vec3 exit_radiance = vec3(0.0);
                for (int event = 0; event < 4 && !shaded; ++event) {
                    hit.surface = invalid_rt_surface();
                    hit.part_slot = hit.primitive = 0xffffffffu;
                    traceRayEXT(scene, gl_RayFlagsOpaqueEXT, 0xff, 1, 0, 1,
                                walk_origin, constants.bias, walk_dir,
                                constants.max_distance, 1);
                    if ((hit.surface.flags & RT_SURFACE_VALID) == 0u) {
                        // Open geometry: no backface found. The authored
                        // thickness stands in for the medium path length.
                        path_length = max(path_length,
                                          trans_material.transmission.z);
                        exit_radiance = environment(walk_dir);
                        shaded = true;
                        break;
                    }
                    path_length += max(hit.surface.hit_t, 0.0);
                    if ((hit.surface.flags & RT_SURFACE_FRONT_FACE) != 0u) {
                        // A solid surface inside/behind the medium blocks
                        // the walk: shade it and stop.
                        vec3 blocked_base;
                        exit_radiance = hit_radiance(hit.surface,
                                                     blocked_base);
                        shaded = true;
                        break;
                    }
                    // Backface: attempt exit refraction. load_rt_surface
                    // flipped the normal to oppose the ray, which is the
                    // orientation refract() expects here.
                    vec3 exit_dir = refract(walk_dir, hit.surface.normal,
                                            ior);
                    if (dot(exit_dir, exit_dir) > 1e-8) {
                        exit_dir = normalize(exit_dir);
                        vec3 exit_origin = hit.surface.position -
                                           hit.surface.normal *
                                               constants.bias;
                        hit.surface = invalid_rt_surface();
                        hit.part_slot = hit.primitive = 0xffffffffu;
                        traceRayEXT(scene, gl_RayFlagsOpaqueEXT, 0xff, 1, 0,
                                    1, exit_origin, constants.bias, exit_dir,
                                    constants.max_distance, 1);
                        vec3 exit_base;
                        exit_radiance =
                            (hit.surface.flags & RT_SURFACE_VALID) != 0u
                                ? hit_radiance(hit.surface, exit_base)
                                : environment(exit_dir);
                        shaded = true;
                        break;
                    }
                    // Total internal reflection: bounce inside the medium
                    // and keep walking.
                    walk_dir = normalize(reflect(walk_dir,
                                                 hit.surface.normal));
                    walk_origin = hit.surface.position +
                                  hit.surface.normal * constants.bias;
                }
                if (!shaded) {
                    // Event cap reached while still inside: fall back to
                    // the environment along the last internal direction.
                    path_length = max(path_length,
                                      trans_material.transmission.z);
                    exit_radiance = environment(walk_dir);
                }
                vec3 absorption_color = trans_material.absorption_pad.rgb;
                // Legacy glass packs black absorption; treat as clear.
                if (dot(absorption_color, absorption_color) < 1e-8)
                    absorption_color = vec3(1.0);
                float absorption_distance = trans_material.transmission.w;
                vec3 transmittance = absorption_distance > 1e-4
                    ? pow(max(absorption_color, vec3(0.0)),
                          vec3(path_length / absorption_distance))
                    : vec3(1.0);
                transmitted = exit_radiance * (1.0 - fresnel) *
                              transmittance;
            }
        }
    }
    if (any(isnan(transmitted)) || any(isinf(transmitted)))
        transmitted = vec3(0.0);
    imageStore(raw_transmission_image, pixel,
               vec4(transmitted, transmission_weight));
```

(This runs after the diffuse and specular lanes have already stored their
results, so reusing the `hit` payload is safe.)

- [ ] **Step 7: composite.frag — binding 8 + blend**

a. After the `identity_texture` binding (Task 1's binding 6 line) — order
in the file: keep the RtMaterialTable block where it is and add below it:

```glsl
layout(set = 0, binding = 8) uniform sampler2D transmission_texture;
```

b. Replace the final combine (the `vec3 linear_hdr = ...; out_hdr = ...;`
lines) with:

```glsl
    vec4 transmission = texture(transmission_texture, in_uv);
    float transmission_coverage = clamp(transmission.a, 0.0, 1.0);
    vec3 linear_hdr = (ambient + sun * mix(1.0, 0.65, roughness) +
                       raw_diffuse) * (1.0 - transmission_coverage) +
                      emission + specular + transmission.rgb;
    out_hdr = vec4(linear_hdr, 1.0);
```

- [ ] **Step 8: Composite layout + descriptor gain binding 8**

In `vk_scene_renderer.cpp`:

a. Task 1's layout block becomes 9 bindings:

```cpp
    std::array<VkDescriptorSetLayoutBinding, 9> sampled_bindings{};
    for (uint32_t i = 0; i < 7; ++i) {
        sampled_bindings[i] = descriptor_binding(
            i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_SHADER_STAGE_FRAGMENT_BIT);
    }
    sampled_bindings[7] = descriptor_binding(
        7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT);
    sampled_bindings[8] = descriptor_binding(
        8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_SHADER_STAGE_FRAGMENT_BIT);
```

b. `update_composite_descriptor` body (Task 1's version) becomes:

```cpp
    matter::VkImageResource* sampled[] = {&albedo_, &normal_, &orm_,
                                          &visibility_, diffuse, specular,
                                          &material_instance_,
                                          &raw_transmission_};
    const uint32_t sampled_slots[] = {0, 1, 2, 3, 4, 5, 6, 8};
    VkDescriptorImageInfo image_infos[8]{};
    VkWriteDescriptorSet writes[9]{};
    for (uint32_t i = 0; i < 8; ++i) {
        image_infos[i].sampler = composite_sampler_;
        image_infos[i].imageView = sampled[i]->view;
        image_infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = frame.composite_descriptor_set;
        writes[i].dstBinding = sampled_slots[i];
        writes[i].descriptorCount = 1;
        writes[i].descriptorType =
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &image_infos[i];
    }
    VkDescriptorBufferInfo material_info{frame.materials.buffer, 0,
                                         frame.materials.size};
    writes[8].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[8].dstSet = frame.composite_descriptor_set;
    writes[8].dstBinding = 7;
    writes[8].descriptorCount = 1;
    writes[8].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[8].pBufferInfo = &material_info;
    vkUpdateDescriptorSets(vulkan_->device(), 9, writes, 0, nullptr);
```

- [ ] **Step 9: Syntax check**

Run CHECK-RENDERER. Expected: exit 0, no output.

- [ ] **Step 10: Commit**

```bash
git add MatterEngine3/src/render/vk_scene_renderer.h \
        MatterEngine3/src/render/vk_scene_renderer.cpp \
        MatterEngine3/shaders_vk/rt_lighting.rgen \
        MatterEngine3/shaders_vk/composite.frag
git commit -m "feat(vulkan): dielectric transmission via deterministic medium walk"
```

---

### Task 3: Thin-walled scattering and wax

Analytic backlight for thin foliage and wrap lighting for wax in
composite.frag, plus the one structural change: thin-scattering geometry
moves to the non-opaque TLAS layer (mask 0x02) so backlit sun rays are
attenuated instead of self-blocked. See spec §4.

**Files:**
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp` (`rt_material_is_opaque`, near line 76)
- Modify: `MatterEngine3/shaders_vk/rt_visibility.rahit` (thin-scattering attenuation branch)
- Modify: `MatterEngine3/shaders_vk/composite.frag` (sun-response restructure)

**Interfaces:**
- Consumes: Task 1's composite `identity_texture` / `rt_materials` bindings and `RtMaterialGpu` struct; `MATERIAL_THIN_WALLED` (1u<<0) and `MATERIAL_ALPHA_TESTED` (1u<<2) from `material_registry.h` (C++) / local GLSL consts; field packing `scattering = [scatteringColor.rgb, subsurface]`, `scattering_shape = [scatteringDistance, anisotropy, alphaCutoff, shadowOpacity]`.
- Produces: no new interfaces; behavioral change only. Materials with `THIN_WALLED && subsurface > 0` (foliageThin 29, leaf) become non-opaque for the shadow TLAS classification.

- [ ] **Step 1: Reclassify thin-scattering materials as non-opaque**

In `vk_scene_renderer.cpp`, replace `rt_material_is_opaque` (near line 76):

```cpp
bool rt_material_is_opaque(const MaterialGpuRecord& material) {
    // Thin-walled scatterers must sit in the non-opaque TLAS layer even
    // though they author shadowOpacity = 1.0: a backlit blob's sun ray
    // starts inside its own geometry, and the opaque layer would
    // self-shadow it to black instead of attenuating (rt_visibility.rahit).
    const bool thin_scattering =
        (material.flags_misc[0] & MATERIAL_THIN_WALLED) != 0u &&
        material.scattering[3] > 0.0f;
    return material.metal_opacity_spec_coat[1] >= 1.0f &&
           material.scattering_shape[3] >= 1.0f &&
           (material.flags_misc[0] & MATERIAL_ALPHA_TESTED) == 0u &&
           material.transmission[0] <= 0.0f && !thin_scattering;
}
```

(`MATERIAL_THIN_WALLED` comes from `material_registry.h`, already included —
same source as the existing `MATERIAL_ALPHA_TESTED` reference. Geometry
classification is recomputed from this function on material upload and TLAS
build; no cache invalidation code is needed.)

- [ ] **Step 2: Attenuate instead of block in rt_visibility.rahit**

In `MatterEngine3/shaders_vk/rt_visibility.rahit`:

a. After `const uint MATERIAL_ALPHA_TESTED = 1u << 2u;` add:

```glsl
const uint MATERIAL_THIN_WALLED = 1u << 0u;
```

b. Replace the attenuation block:

```glsl
    vec3 transmission_tint = clamp(material.base_roughness.rgb *
                                   material.transmission.x, 0.0, 1.0);
    float shadow_opacity =
        clamp(material.scattering_shape.w, 0.0, 1.0);
    payload.visibility *= mix(vec3(1.0), transmission_tint, shadow_opacity);
```

with:

```glsl
    float subsurface = clamp(material.scattering.w, 0.0, 1.0);
    if ((material.flags_misc.x & MATERIAL_THIN_WALLED) != 0u &&
        subsurface > 0.0) {
        // Thin scatterers author shadowOpacity = 1.0, which would fully
        // block; the transmitted fraction is scattering color x subsurface.
        payload.visibility *= clamp(material.scattering.rgb * subsurface,
                                    0.0, 1.0);
    } else {
        vec3 transmission_tint = clamp(material.base_roughness.rgb *
                                       material.transmission.x, 0.0, 1.0);
        float shadow_opacity =
            clamp(material.scattering_shape.w, 0.0, 1.0);
        payload.visibility *= mix(vec3(1.0), transmission_tint,
                                  shadow_opacity);
    }
```

The layer counting / termination code after this block is unchanged.

- [ ] **Step 3: composite.frag — thin backlight and wax wrap**

Two edits to `composite.frag` (state after Task 2):

a. After the `RtMaterialTable` buffer block, add:

```glsl
const uint MATERIAL_THIN_WALLED = 1u << 0u;
```

b. In `main()`, replace this sequence (the sun term plus the
`material_index` fetch that currently sits in the emission section):

```glsl
    vec3 sun = diffuse * lighting.sun_color * direct * lighting.sun_intensity *
               visibility;
```

and, further down, remove the line

```glsl
    uint material_index = texelFetch(identity_texture,
                                     ivec2(gl_FragCoord.xy), 0).r;
```

from the emission section, replacing the former with:

```glsl
    uint material_index = texelFetch(identity_texture,
                                     ivec2(gl_FragCoord.xy), 0).r;
    vec3 sun_response = diffuse * direct;
    if (material_index < rt_materials.length()) {
        RtMaterialGpu material = rt_materials[material_index];
        float subsurface = clamp(material.scattering.w, 0.0, 1.0);
        if (subsurface > 0.0) {
            if ((material.flags_misc.x & MATERIAL_THIN_WALLED) != 0u) {
                // Thin-walled backlight: view-independent wrapped term
                // (composite has no camera position), sharpened by the
                // authored anisotropy.
                float backlit = clamp(-dot(normal, to_sun), 0.0, 1.0);
                float aniso = clamp(material.scattering_shape.y, 0.0, 1.0);
                backlit = pow(backlit, mix(1.0, 4.0, aniso));
                float distance_falloff =
                    clamp(material.scattering_shape.x * 4.0, 0.0, 1.0);
                sun_response += material.scattering.rgb * subsurface *
                                backlit * distance_falloff;
            } else {
                // Wax-style wrap: soften the terminator; the wrapped-in
                // region is tinted by the scattering color.
                float wrapped = clamp((dot(normal, to_sun) + subsurface) /
                                      (1.0 + subsurface), 0.0, 1.0);
                sun_response += diffuse * material.scattering.rgb *
                                max(wrapped - direct, 0.0);
            }
        }
    }
    vec3 sun = sun_response * lighting.sun_color * lighting.sun_intensity *
               visibility;
```

The emission section keeps using `material_index` (now defined earlier);
its remaining lines are unchanged. Both new terms are multiplied by the
shadow visibility texture via `sun` — for backlit foliage that visibility
now carries the rahit tint from Step 2 instead of hard zero. (Per spec §4:
a closed blob whose sun ray self-hits gets the scattering tint applied by
both rahit and the analytic term; that is the authored thin-walled contract,
and final tuning comes from Jack's runtime feedback. The `* 4.0` in
`distance_falloff` is a first-pass mapping of scatteringDistance ~0.18 →
~0.72; flag it in the commit body as tunable, do not add a UI control.)

- [ ] **Step 4: Syntax check**

Run CHECK-RENDERER. Expected: exit 0, no output.

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/src/render/vk_scene_renderer.cpp \
        MatterEngine3/shaders_vk/rt_visibility.rahit \
        MatterEngine3/shaders_vk/composite.frag
git commit -m "feat(vulkan): thin-walled backlight and wax wrap shading"
```

---

### Task 4: Second diffuse GI bounce

Restructure the diffuse GI section of rt_lighting.rgen into a 2-vertex loop
with a running throughput. Independently revertible if Jack's testing finds
the noise or cost unacceptable. See spec §6. No renderer-side change: the
signal, image, and denoiser chain are untouched.

**Files:**
- Modify: `MatterEngine3/shaders_vk/rt_lighting.rgen` (diffuse GI section only)

**Interfaces:**
- Consumes: `hit_emission(RtSurface, out vec3)` and `hit_radiance(RtSurface, out vec3)` from Task 1; existing `cosine_direction`, `environment`, two-layer shadow-trace pattern, `visibility_payload`.
- Produces: nothing new — same `raw_diffuse_image` output, +1 diffuse ray + NEE per pixel. Explicitly excluded (spec §6/§9): Russian roulette, adaptive quality, ray instrumentation.

- [ ] **Step 1: Replace the single-bounce diffuse section with a 2-vertex loop**

In `MatterEngine3/shaders_vk/rt_lighting.rgen` `main()`, find the block that
starts at `vec3 direction = cosine_direction(normal, seed);` and ends with
the closing `else { bounce = environment(direction); }` (originally lines
195-232 — after Task 2 the numbers have shifted; match the text). Replace
that entire block — from `vec3 direction = ...` through the `}` closing the
`else` — with:

```glsl
    vec3 bounce = vec3(0.0);
    vec3 throughput = vec3(1.0);
    vec3 vertex_origin = world + normal * constants.bias;
    vec3 vertex_normal = normal;
    vec3 to_sun = normalize(constants.to_sun);
    for (int vertex = 0; vertex < 2; ++vertex) {
        vec3 direction = cosine_direction(vertex_normal, seed);
        hit.surface = invalid_rt_surface();
        hit.part_slot = hit.primitive = 0xffffffffu;
        traceRayEXT(scene, gl_RayFlagsOpaqueEXT, 0xff, 1, 0, 1,
                    vertex_origin, constants.bias, direction,
                    constants.max_distance, 1);
        if ((hit.surface.flags & RT_SURFACE_VALID) == 0u) {
            bounce += throughput * environment(direction);
            break;
        }
        vec3 hit_base;
        bool final_vertex = vertex == 1;
        // sky_irradiance approximates everything beyond the final vertex;
        // adding it at the intermediate vertex too would double count the
        // sky, so intermediate vertices contribute emission only.
        bounce += throughput * (final_vertex
                                    ? hit_radiance(hit.surface, hit_base)
                                    : hit_emission(hit.surface, hit_base));
        float ndotl = max(dot(hit.surface.normal, to_sun), 0.0);
        if (ndotl > 0.0 && any(greaterThan(hit_base, vec3(0.0)))) {
            visibility_payload.visibility = vec3(1.0);
            visibility_payload.layers = 0u;
            traceRayEXT(scene, gl_RayFlagsTerminateOnFirstHitEXT |
                               gl_RayFlagsOpaqueEXT,
                        0x01, 0, 0, 0,
                        hit.surface.position +
                            hit.surface.normal * constants.bias,
                        constants.bias, to_sun, constants.max_distance, 0);
            if (max(visibility_payload.visibility.r,
                    max(visibility_payload.visibility.g,
                        visibility_payload.visibility.b)) >= 0.01) {
                traceRayEXT(scene, gl_RayFlagsNoneEXT, 0x02, 0, 0, 0,
                            hit.surface.position +
                                hit.surface.normal * constants.bias,
                            constants.bias, to_sun,
                            constants.max_distance, 0);
            }
            // Lambertian 1/pi keeps the sun-lit indirect component in
            // ratio with the sky component (whose pi cancels against the
            // cosine-sampling pdf).
            bounce += throughput * hit_base * 0.31830988618 *
                      constants.sun_color * constants.sun_intensity * ndotl *
                      visibility_payload.visibility;
        }
        if (final_vertex) break;
        throughput *= hit_base;
        if (max(throughput.r, max(throughput.g, throughput.b)) < 1e-3)
            break;
        vertex_origin = hit.surface.position +
                        hit.surface.normal * constants.bias;
        vertex_normal = hit.surface.normal;
    }
```

Notes for the implementer:
- The original block declared `vec3 bounce;` unassigned and `vec3 to_sun`
  inside the hit branch; both are now declared up front. Ensure no duplicate
  `bounce`/`to_sun` declarations remain in the replaced region.
- The lines following the loop (`vec3 primary_diffuse = ...` through
  `imageStore(raw_diffuse_image, ...)`) are unchanged: with one vertex the
  loop reproduces the old estimator exactly (throughput = 1 at vertex 0,
  full `hit_radiance` at the final vertex).
- The specular section below still declares its own hit/trace state and is
  untouched; it reuses the `hit` payload after the loop, which stays safe.

- [ ] **Step 2: Commit**

```bash
git add MatterEngine3/shaders_vk/rt_lighting.rgen
git commit -m "feat(vulkan): second diffuse GI bounce with throughput loop"
```

---

## Execution workflow (from spec §8)

- Sonnet subagents execute each task from these self-contained steps.
- After each task, an **adversarial opus subagent** reviews the actual diff
  (`git show`) against the spec section and this plan's task, hunting for
  bugs, spec deviations, descriptor/layout mismatches, and GLSL errors —
  shaders cannot be compiled locally, so the review is the only shader gate.
- The main agent verifies claims, resolves review findings, and commits.
- After Task 4: Jack runs the MSYS2 Windows build (regenerating
  `embedded_spirv.h`, which also covers the previously stacked shader
  commits) and does all runtime testing in the garden scene, returning
  screenshots and feedback for tuning.
