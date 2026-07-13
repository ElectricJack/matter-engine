#include "rt_params.h"
#include <optix_device.h>
#include <cuda_fp16.h>

extern "C" __constant__ RtLaunchParams params;

// Simple hash-based PRNG for stochastic ray directions.
__device__ unsigned int tea(unsigned int val0, unsigned int val1) {
    unsigned int v0 = val0, v1 = val1, s0 = 0;
    for (int n = 0; n < 4; ++n) {
        s0 += 0x9e3779b9;
        v0 += ((v1 << 4) + 0xa341316c) ^ (v1 + s0) ^ ((v1 >> 5) + 0xc8013ea4);
        v1 += ((v0 << 4) + 0xad90777d) ^ (v0 + s0) ^ ((v0 >> 5) + 0x7e95761e);
    }
    return v0;
}

__device__ float rng(unsigned int& seed) {
    seed = seed * 1664525u + 1013904223u;
    return (seed & 0x00ffffff) / (float)0x01000000;
}

__device__ float3 cosine_hemisphere(float3 N, unsigned int& seed) {
    float u1 = rng(seed);
    float u2 = rng(seed);
    float r = sqrtf(u1);
    float phi = 6.28318530718f * u2;
    float x = r * cosf(phi);
    float y = r * sinf(phi);
    float z = sqrtf(fmaxf(0.0f, 1.0f - u1));

    float3 w = N;
    float3 a = (fabsf(w.x) > 0.1f) ? make_float3(0,1,0) : make_float3(1,0,0);
    float3 u_vec = make_float3(a.y*w.z - a.z*w.y, a.z*w.x - a.x*w.z, a.x*w.y - a.y*w.x);
    float len = sqrtf(u_vec.x*u_vec.x + u_vec.y*u_vec.y + u_vec.z*u_vec.z);
    u_vec.x /= len; u_vec.y /= len; u_vec.z /= len;
    float3 v_vec = make_float3(w.y*u_vec.z - w.z*u_vec.y, w.z*u_vec.x - w.x*u_vec.z, w.x*u_vec.y - w.y*u_vec.x);

    return make_float3(
        u_vec.x*x + v_vec.x*y + w.x*z,
        u_vec.y*x + v_vec.y*y + w.y*z,
        u_vec.z*x + v_vec.z*y + w.z*z);
}

__device__ float3 reflect_dir(float3 I, float3 N) {
    float d = I.x*N.x + I.y*N.y + I.z*N.z;
    return make_float3(I.x - 2*d*N.x, I.y - 2*d*N.y, I.z - 2*d*N.z);
}

__device__ bool trace_shadow(float3 origin, float3 dir) {
    unsigned int hit = 0;
    optixTrace(params.tlas, origin, dir,
               0.5f, 1000.0f, 0.0f, 0xFF,
               OPTIX_RAY_FLAG_TERMINATE_ON_FIRST_HIT | OPTIX_RAY_FLAG_DISABLE_CLOSESTHIT,
               0, 2, 0,  // ray type 0, stride 2, miss index 0
               hit);
    return hit != 0;
}

struct RadianceResult {
    float3 albedo;
    float3 normal;
};

__device__ RadianceResult trace_radiance(float3 origin, float3 dir, float tmin, float tmax) {
    unsigned int p0=0, p1=0, p2=0, p3=0, p4=0, p5=0;
    optixTrace(params.tlas, origin, dir,
               tmin, tmax, 0.0f, 0xFF,
               OPTIX_RAY_FLAG_NONE,
               1, 2, 1,  // ray type 1, stride 2, miss index 1
               p0, p1, p2, p3, p4, p5);
    RadianceResult r;
    r.albedo = make_float3(__uint_as_float(p0), __uint_as_float(p1), __uint_as_float(p2));
    r.normal = make_float3(__uint_as_float(p3), __uint_as_float(p4), __uint_as_float(p5));
    return r;
}

extern "C" __global__ void __raygen__lighting() {
    uint3 idx = optixGetLaunchIndex();
    if (idx.x >= (unsigned)params.width || idx.y >= (unsigned)params.height) return;

    // Read depth from half-res linearized depth surface.
    float z_ndc;
    surf2Dread(&z_ndc, (cudaSurfaceObject_t)params.depth_surface,
               idx.x * sizeof(float), idx.y);

    if (z_ndc >= 0.9999f) {
        // Sky pixel: output sky color directly as lighting.
        // Reconstruct the view direction for sky sampling.
        float ndc_x = ((float)idx.x + 0.5f) / (float)params.width  * 2.0f - 1.0f;
        float ndc_y = ((float)idx.y + 0.5f) / (float)params.height * 2.0f - 1.0f;
        const float* m = params.inv_vp;
        float far_x = ndc_x*m[0] + ndc_y*m[4] + 1.0f*m[8] + m[12];
        float far_y = ndc_x*m[1] + ndc_y*m[5] + 1.0f*m[9] + m[13];
        float far_z = ndc_x*m[2] + ndc_y*m[6] + 1.0f*m[10]+ m[14];
        float far_w = ndc_x*m[3] + ndc_y*m[7] + 1.0f*m[11]+ m[15];
        float3 far_pt = make_float3(far_x/far_w, far_y/far_w, far_z/far_w);
        float near_x = ndc_x*m[0] + ndc_y*m[4] + (-1.0f)*m[8] + m[12];
        float near_y = ndc_x*m[1] + ndc_y*m[5] + (-1.0f)*m[9] + m[13];
        float near_z = ndc_x*m[2] + ndc_y*m[6] + (-1.0f)*m[10]+ m[14];
        float near_w = ndc_x*m[3] + ndc_y*m[7] + (-1.0f)*m[11]+ m[15];
        float3 near_pt = make_float3(near_x/near_w, near_y/near_w, near_z/near_w);
        float3 view_dir = make_float3(far_pt.x-near_pt.x, far_pt.y-near_pt.y, far_pt.z-near_pt.z);
        float vlen = sqrtf(view_dir.x*view_dir.x + view_dir.y*view_dir.y + view_dir.z*view_dir.z);
        if (vlen > 1e-6f) { view_dir.x/=vlen; view_dir.y/=vlen; view_dir.z/=vlen; }

        // Sample sky (same as lighting_miss.cu).
        float height = view_dir.y;
        float3 zenith  = make_float3(0.25f, 0.5f, 1.0f);
        float3 horizon = make_float3(0.9f, 0.7f, 0.5f);
        float3 ground  = make_float3(0.3f, 0.25f, 0.2f);
        float3 sky;
        if (height > 0.0f) {
            float t = fminf(height / 0.6f, 1.0f);
            t = t*t*(3.0f - 2.0f*t);
            sky.x = horizon.x + (zenith.x-horizon.x)*t;
            sky.y = horizon.y + (zenith.y-horizon.y)*t;
            sky.z = horizon.z + (zenith.z-horizon.z)*t;
        } else {
            float d = fminf(-height * 2.0f, 1.0f);
            sky.x = horizon.x*0.4f + (ground.x - horizon.x*0.4f)*d;
            sky.y = horizon.y*0.4f + (ground.y - horizon.y*0.4f)*d;
            sky.z = horizon.z*0.4f + (ground.z - horizon.z*0.4f)*d;
        }
        sky.x *= params.sky_color[0]/0.38f;
        sky.y *= params.sky_color[1]/0.43f;
        sky.z *= params.sky_color[2]/0.52f;

        float4 out_val = make_float4(sky.x, sky.y, sky.z, 1.0f);
        surf2Dwrite(out_val, (cudaSurfaceObject_t)params.lighting_surface,
                    idx.x * sizeof(float4), idx.y);
        return;
    }

    // Reconstruct world position from depth.
    float ndc_x = ((float)idx.x + 0.5f) / (float)params.width  * 2.0f - 1.0f;
    float ndc_y = ((float)idx.y + 0.5f) / (float)params.height * 2.0f - 1.0f;
    float ndc_z = z_ndc;
    const float* m = params.inv_vp;
    float cx = ndc_x*m[0] + ndc_y*m[4] + ndc_z*m[8]  + m[12];
    float cy = ndc_x*m[1] + ndc_y*m[5] + ndc_z*m[9]  + m[13];
    float cz = ndc_x*m[2] + ndc_y*m[6] + ndc_z*m[10] + m[14];
    float cw = ndc_x*m[3] + ndc_y*m[7] + ndc_z*m[11] + m[15];
    float3 world_pos = make_float3(cx/cw, cy/cw, cz/cw);

    // Sample G-buffer at corresponding full-res coordinate.
    int gx = (int)((float)idx.x / (float)params.width  * (float)params.screen_w);
    int gy = (int)((float)idx.y / (float)params.height * (float)params.screen_h);
    gx = min(gx, params.screen_w - 1);
    gy = min(gy, params.screen_h - 1);

    // Albedo + emission from G-buffer RT0 (RGBA8 → uchar4).
    uchar4 alb_raw;
    surf2Dread(&alb_raw, (cudaSurfaceObject_t)params.albedo_surface,
               gx * (int)sizeof(uchar4), gy);
    float3 albedo = make_float3(alb_raw.x / 255.0f, alb_raw.y / 255.0f, alb_raw.z / 255.0f);
    float emission = alb_raw.w / 255.0f;

    // Normal + translucency from G-buffer RT1 (RGBA16F).
    ushort4 norm_raw;
    surf2Dread(&norm_raw, (cudaSurfaceObject_t)params.normal_surface,
               gx * (int)sizeof(ushort4), gy);
    float3 N = make_float3(
        __half2float(*((__half*)&norm_raw.x)),
        __half2float(*((__half*)&norm_raw.y)),
        __half2float(*((__half*)&norm_raw.z)));
    N.x = N.x * 2.0f - 1.0f;  // decode from [0,1] to [-1,1]
    N.y = N.y * 2.0f - 1.0f;
    N.z = N.z * 2.0f - 1.0f;
    float nlen = sqrtf(N.x*N.x + N.y*N.y + N.z*N.z);
    if (nlen > 1e-6f) { N.x/=nlen; N.y/=nlen; N.z/=nlen; }
    float translucency = __half2float(*((__half*)&norm_raw.w));

    // ORM from G-buffer RT2 (RGBA8 → uchar4).
    uchar4 orm_raw;
    surf2Dread(&orm_raw, (cudaSurfaceObject_t)params.orm_surface,
               gx * (int)sizeof(uchar4), gy);
    float roughness = orm_raw.x / 255.0f;
    float metallic  = orm_raw.y / 255.0f;
    float ao        = orm_raw.z / 255.0f;

    // Initialize RNG per pixel.
    unsigned int seed = tea(idx.x + idx.y * params.width, params.frame_index);

    float3 sun_dir = make_float3(params.sun_dir[0], params.sun_dir[1], params.sun_dir[2]);
    float3 sun_col = make_float3(params.sun_color[0], params.sun_color[1], params.sun_color[2]);

    // 1. Direct sun shadow.
    float sun_vis = trace_shadow(world_pos, sun_dir) ? 0.0f : 1.0f;
    float ndl = fmaxf(0.0f, N.x*sun_dir.x + N.y*sun_dir.y + N.z*sun_dir.z);

    // Simple PBR: F0 for dielectrics = 0.04, for metals = albedo.
    float3 F0 = make_float3(
        metallic * albedo.x + (1-metallic) * 0.04f,
        metallic * albedo.y + (1-metallic) * 0.04f,
        metallic * albedo.z + (1-metallic) * 0.04f);

    // Diffuse component (Lambert).
    float3 kD = make_float3((1-metallic), (1-metallic), (1-metallic));
    float3 direct = make_float3(
        kD.x * albedo.x / 3.14159f * sun_col.x * ndl * sun_vis,
        kD.y * albedo.y / 3.14159f * sun_col.y * ndl * sun_vis,
        kD.z * albedo.z / 3.14159f * sun_col.z * ndl * sun_vis);

    // 2. Indirect GI bounce (1 ray, cosine-weighted hemisphere).
    float3 indirect = make_float3(0, 0, 0);
    {
        float3 bounce_dir = cosine_hemisphere(N, seed);
        RadianceResult hit = trace_radiance(world_pos, bounce_dir, 0.5f, 100.0f);

        // Check if this is a sky miss (normal == ray direction, a convention from lighting_miss).
        float hit_ndl_check = hit.normal.x*bounce_dir.x + hit.normal.y*bounce_dir.y + hit.normal.z*bounce_dir.z;
        bool is_sky = (hit_ndl_check > 0.99f);

        if (is_sky) {
            // Sky illumination.
            indirect.x = hit.albedo.x * albedo.x * kD.x;
            indirect.y = hit.albedo.y * albedo.y * kD.y;
            indirect.z = hit.albedo.z * albedo.z * kD.z;
        } else {
            // Surface hit: compute direct sun at bounce point, modulate by hit albedo.
            // The closest-hit doesn't return distance, so approximate the hit point
            // by using a fixed offset from origin along bounce direction.
            float bounce_sun_vis = trace_shadow(
                make_float3(
                    world_pos.x + bounce_dir.x * 50.0f,
                    world_pos.y + bounce_dir.y * 50.0f,
                    world_pos.z + bounce_dir.z * 50.0f),
                sun_dir) ? 0.0f : 1.0f;
            float bounce_ndl = fmaxf(0.0f,
                hit.normal.x*sun_dir.x + hit.normal.y*sun_dir.y + hit.normal.z*sun_dir.z);

            indirect.x = hit.albedo.x * bounce_ndl * bounce_sun_vis * sun_col.x * albedo.x * kD.x * 0.5f;
            indirect.y = hit.albedo.y * bounce_ndl * bounce_sun_vis * sun_col.y * albedo.y * kD.y * 0.5f;
            indirect.z = hit.albedo.z * bounce_ndl * bounce_sun_vis * sun_col.z * albedo.z * kD.z * 0.5f;
        }
    }

    // 3. Reflection (if roughness < 0.3 and metallic > 0.1).
    float3 reflection = make_float3(0, 0, 0);
    if (roughness < 0.3f) {
        // Reconstruct view direction from camera → world_pos.
        float near_x2 = ndc_x*m[0] + ndc_y*m[4] + (-1.0f)*m[8]  + m[12];
        float near_y2 = ndc_x*m[1] + ndc_y*m[5] + (-1.0f)*m[9]  + m[13];
        float near_z2 = ndc_x*m[2] + ndc_y*m[6] + (-1.0f)*m[10] + m[14];
        float near_w2 = ndc_x*m[3] + ndc_y*m[7] + (-1.0f)*m[11] + m[15];
        float3 cam_pos = make_float3(near_x2/near_w2, near_y2/near_w2, near_z2/near_w2);
        float3 view_dir = make_float3(
            world_pos.x - cam_pos.x,
            world_pos.y - cam_pos.y,
            world_pos.z - cam_pos.z);
        float vlen = sqrtf(view_dir.x*view_dir.x + view_dir.y*view_dir.y + view_dir.z*view_dir.z);
        if (vlen > 1e-6f) { view_dir.x/=vlen; view_dir.y/=vlen; view_dir.z/=vlen; }

        float3 refl = reflect_dir(view_dir, N);
        // Add roughness jitter.
        refl.x += (rng(seed) - 0.5f) * roughness;
        refl.y += (rng(seed) - 0.5f) * roughness;
        refl.z += (rng(seed) - 0.5f) * roughness;
        float rlen = sqrtf(refl.x*refl.x + refl.y*refl.y + refl.z*refl.z);
        if (rlen > 1e-6f) { refl.x/=rlen; refl.y/=rlen; refl.z/=rlen; }

        RadianceResult refl_hit = trace_radiance(world_pos, refl, 0.5f, 500.0f);

        // Schlick Fresnel.
        float cos_i = fabsf(view_dir.x*N.x + view_dir.y*N.y + view_dir.z*N.z);
        float3 F = make_float3(
            F0.x + (1-F0.x) * powf(1-cos_i, 5.0f),
            F0.y + (1-F0.y) * powf(1-cos_i, 5.0f),
            F0.z + (1-F0.z) * powf(1-cos_i, 5.0f));

        reflection.x = refl_hit.albedo.x * F.x;
        reflection.y = refl_hit.albedo.y * F.y;
        reflection.z = refl_hit.albedo.z * F.z;
    }

    // 4. SSS/Translucency (if translucency > 0).
    float3 sss = make_float3(0, 0, 0);
    if (translucency > 0.01f) {
        // Check if sun is visible from behind (backlit leaf glow).
        // SSS uses sun shadow + backface NdotL only; no radiance trace needed.
        float back_sun_vis = trace_shadow(world_pos, sun_dir) ? 0.0f : 1.0f;
        float back_ndl = fmaxf(0.0f, -(N.x*sun_dir.x + N.y*sun_dir.y + N.z*sun_dir.z));

        sss.x = albedo.x * translucency * (back_ndl * sun_col.x * 0.5f);
        sss.y = albedo.y * translucency * (back_ndl * sun_col.y * 0.5f);
        sss.z = albedo.z * translucency * (back_ndl * sun_col.z * 0.5f);
    }

    // Sky ambient.
    float sky_factor = fmaxf(0.0f, N.y) * 0.15f;
    float3 ambient = make_float3(
        params.sky_color[0] * albedo.x * kD.x * ao * sky_factor,
        params.sky_color[1] * albedo.y * kD.y * ao * sky_factor,
        params.sky_color[2] * albedo.z * kD.z * ao * sky_factor);

    // Combine all lighting.
    float3 lighting = make_float3(
        direct.x + indirect.x + reflection.x + sss.x + ambient.x + albedo.x * emission,
        direct.y + indirect.y + reflection.y + sss.y + ambient.y + albedo.y * emission,
        direct.z + indirect.z + reflection.z + sss.z + ambient.z + albedo.z * emission);

    float4 out_val = make_float4(lighting.x, lighting.y, lighting.z, 1.0f);
    surf2Dwrite(out_val, (cudaSurfaceObject_t)params.lighting_surface,
                idx.x * sizeof(float4), idx.y);
}
