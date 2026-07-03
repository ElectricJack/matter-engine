#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace probe_volume {

struct ProbeGrid {
    float origin[3] = {0,0,0};   // world position of cell (0,0,0) CENTER
    float cell      = 1.0f;
    int   nx = 0, ny = 0, nz = 0;
};

// Float storage on CPU; quantization to RGBA8 happens at GPU upload (Task 6).
// Layout x-fastest: idx = ((z*ny)+y)*nx + x, 4 floats per cell.
struct ProbeVolume {
    ProbeGrid grid;
    std::vector<float> ambient;   // rgb = ambient irradiance, a = sun visibility [0,1]
    std::vector<float> dominant;  // xyz = dominant incoming-light dir (unit or 0), a = intensity
    size_t cells() const { return (size_t)grid.nx * grid.ny * grid.nz; }
    bool   valid() const { return cells() > 0 && ambient.size() == cells()*4
                                              && dominant.size() == cells()*4; }
};

// 'PRB1' file: {magic u32, fingerprint u64, ProbeGrid, content fnv1a64 over both
// blobs} header + raw float blobs. Atomic tmp+rename (same pattern as save_v2).
// load returns false on magic/size/fingerprint/content-hash mismatch.
bool save_probes(const std::string& path, const ProbeVolume& v, uint64_t fingerprint);
bool load_probes(const std::string& path, ProbeVolume& out, uint64_t expected_fingerprint);

} // namespace probe_volume
