#ifndef VT_NOISE_GLSL
#define VT_NOISE_GLSL

// vt_noise.glsl — the GLSL twin of terrain_field.cpp's noise core.
//
// Lifted VERBATIM out of vt_surface_tape.glsl (which still includes it and is
// still its primary consumer) so a second shader can use the same value-noise
// and fbm without a second implementation of either. vol_density.comp carves
// its cloud layers with vt_fbm3 from here.
//
// The extraction is text-identical: the SPIR-V glslc emits for
// vt_surface_tape.glsl is byte-for-byte unchanged by it, which is the only
// acceptable outcome for a file whose determinism is a bake gate.
//
// DETERMINISM (spec §4.4), unchanged by the move:
//   * hash2i/hash3i are pure uint arithmetic with the exact CPU constants and
//     operation order — bit-exact vs the CPU by construction.
//   * rand01 mirrors the exact CPU conversion ((hash & 0xffffff) / 0x1000000).
//   * The float lerp/fbm chains reproduce the CPU float operation ORDER;
//     `precise` on the accumulators blocks fma contraction from reordering
//     them.
//
// Keep terrain_field.cpp, this file, and any CPU twin in lockstep.

// ---- noise twin (terrain_field.cpp anonymous namespace, line for line) ----

uint vt_hash2i(int ix, int iz, uint seed) {
    uint h = uint(ix) * 374761393u + uint(iz) * 668265263u
           + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

float vt_rand01(int ix, int iz, uint seed) {
    return float(vt_hash2i(ix, iz, seed) & 0xffffffu) / 16777216.0;
}

float vt_smooth5(float t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }

float vt_value_noise(float x, float z, uint seed) {
    float fx0 = floor(x), fz0 = floor(z);
    int ix = int(fx0), iz = int(fz0);
    float fx = x - fx0, fz = z - fz0;
    float a = vt_rand01(ix,     iz,     seed);
    float b = vt_rand01(ix + 1, iz,     seed);
    float c = vt_rand01(ix,     iz + 1, seed);
    float d = vt_rand01(ix + 1, iz + 1, seed);
    float u = vt_smooth5(fx), v = vt_smooth5(fz);
    precise float r = (a + (b - a) * u) * (1.0 - v) + (c + (d - c) * u) * v;
    return r;   // 0..1
}

float vt_fbm2(float x, float z, uint seed, int oct, float gain, float lac,
              float freq, bool ridged) {
    precise float amp = 1.0;
    precise float sum = 0.0;
    precise float norm = 0.0;
    for (int i = 0; i < oct; ++i) {
        float n = vt_value_noise(x * freq, z * freq, seed + uint(i) * 131u);
        n = n * 2.0 - 1.0;                        // -1..1
        if (ridged) n = 1.0 - abs(n) * 2.0;       // ridge: peaks at lattice
        sum += n * amp; norm += amp;
        amp *= gain; freq *= lac;
    }
    return sum / norm;   // ~-1..1
}

// 3D lattice hash: hash2i's avalanche mix + PRIME32_3 for the y fold.
uint vt_hash3i(int ix, int iy, int iz, uint seed) {
    uint h = uint(ix) * 374761393u + uint(iy) * 3266489917u
           + uint(iz) * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

float vt_rand01_3(int ix, int iy, int iz, uint seed) {
    return float(vt_hash3i(ix, iy, iz, seed) & 0xffffffu) / 16777216.0;
}

float vt_value_noise3(float x, float y, float z, uint seed) {
    float fx0 = floor(x), fy0 = floor(y), fz0 = floor(z);
    int ix = int(fx0), iy = int(fy0), iz = int(fz0);
    float fx = x - fx0, fy = y - fy0, fz = z - fz0;
    // Trilinear over the 8 cell corners, same smoothstep fade as 2D.
    float c000 = vt_rand01_3(ix,     iy,     iz,     seed);
    float c100 = vt_rand01_3(ix + 1, iy,     iz,     seed);
    float c010 = vt_rand01_3(ix,     iy + 1, iz,     seed);
    float c110 = vt_rand01_3(ix + 1, iy + 1, iz,     seed);
    float c001 = vt_rand01_3(ix,     iy,     iz + 1, seed);
    float c101 = vt_rand01_3(ix + 1, iy,     iz + 1, seed);
    float c011 = vt_rand01_3(ix,     iy + 1, iz + 1, seed);
    float c111 = vt_rand01_3(ix + 1, iy + 1, iz + 1, seed);
    float u = vt_smooth5(fx), v = vt_smooth5(fy), w = vt_smooth5(fz);
    precise float x00 = c000 + (c100 - c000) * u;
    precise float x10 = c010 + (c110 - c010) * u;
    precise float x01 = c001 + (c101 - c001) * u;
    precise float x11 = c011 + (c111 - c011) * u;
    precise float y0 = x00 + (x10 - x00) * v;
    precise float y1 = x01 + (x11 - x01) * v;
    precise float r = y0 + (y1 - y0) * w;
    return r;   // 0..1
}

float vt_fbm3(float x, float y, float z, uint seed, int oct, float gain,
              float lac, float freq, bool ridged) {
    precise float amp = 1.0;
    precise float sum = 0.0;
    precise float norm = 0.0;
    for (int i = 0; i < oct; ++i) {
        float n = vt_value_noise3(x * freq, y * freq, z * freq,
                                  seed + uint(i) * 131u);
        n = n * 2.0 - 1.0;                        // -1..1
        if (ridged) n = 1.0 - abs(n) * 2.0;       // ridge: peaks at lattice
        sum += n * amp; norm += amp;
        amp *= gain; freq *= lac;
    }
    return sum / norm;   // ~-1..1
}

#endif  // VT_NOISE_GLSL
