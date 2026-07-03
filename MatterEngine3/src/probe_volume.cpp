// probe_volume.cpp — PRB1 file serialization for ProbeVolume.
//
// File layout (all fields written field-by-field, little-endian native byte order):
//   u32  magic        = 0x31425250  ('PRB1' in ASCII, little-endian: P=50,R=52,B=42,1=31)
//   u64  fingerprint  (scene/grid/lights identity hash supplied by the caller)
//   f32  origin[3]
//   f32  cell
//   i32  nx, ny, nz
//   u64  content_hash (fnv1a64 over the concatenation of ambient bytes then dominant bytes —
//                      computed by hashing a single temp buffer to keep it deterministic
//                      regardless of any future reordering of the struct fields)
//   f32  ambient[cells*4]
//   f32  dominant[cells*4]
//
// Write is atomic: written to path+".tmp", then renamed to path (mirrors save_v2 in
// part_asset_v2.cpp).

#include "probe_volume.h"
#include "part_asset.h"   // fnv1a64

#include <cstdio>
#include <cstring>
#include <vector>

namespace probe_volume {

static constexpr uint32_t PRB1_MAGIC = 0x31425250u; // 'PRB1'

// Header size in bytes (fields before the float blobs):
//   u32 magic + u64 fingerprint + f32 origin[3] + f32 cell + i32 nx + i32 ny + i32 nz + u64 content_hash
static constexpr size_t HEADER_SIZE =
    sizeof(uint32_t)    // magic
  + sizeof(uint64_t)    // fingerprint
  + sizeof(float) * 3   // origin
  + sizeof(float)       // cell
  + sizeof(int32_t) * 3 // nx, ny, nz
  + sizeof(uint64_t);   // content_hash

bool save_probes(const std::string& path, const ProbeVolume& v, uint64_t fingerprint) {
    if (!v.valid()) return false;

    // Content hash: fnv1a64 over the concatenation of ambient bytes then dominant bytes.
    // Using a single temp buffer ensures the hash is deterministic independent of any
    // future struct-layout changes. Hash covers exactly the bytes that will be written.
    const size_t amb_bytes = v.ambient.size()  * sizeof(float);
    const size_t dom_bytes = v.dominant.size() * sizeof(float);
    std::vector<uint8_t> content_buf(amb_bytes + dom_bytes);
    std::memcpy(content_buf.data(),            v.ambient.data(),  amb_bytes);
    std::memcpy(content_buf.data() + amb_bytes, v.dominant.data(), dom_bytes);
    const uint64_t content_hash = part_asset::fnv1a64(content_buf.data(), content_buf.size());

    // Atomic write: write to tmp then rename.
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;

    // Helper lambda to fwrite a value and accumulate ok flag.
    bool ok = true;
    auto put = [&](const void* p, size_t n) {
        ok = ok && (std::fwrite(p, 1, n, f) == n);
    };

    const int32_t nx = v.grid.nx, ny = v.grid.ny, nz = v.grid.nz;

    put(&PRB1_MAGIC,    sizeof(PRB1_MAGIC));
    put(&fingerprint,   sizeof(fingerprint));
    put(v.grid.origin,  sizeof(v.grid.origin));
    put(&v.grid.cell,   sizeof(v.grid.cell));
    put(&nx,            sizeof(nx));
    put(&ny,            sizeof(ny));
    put(&nz,            sizeof(nz));
    put(&content_hash,  sizeof(content_hash));
    // Float blobs.
    put(v.ambient.data(),  amb_bytes);
    put(v.dominant.data(), dom_bytes);

    std::fclose(f);
    if (!ok) { std::remove(tmp.c_str()); return false; }
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

bool load_probes(const std::string& path, ProbeVolume& out, uint64_t expected_fingerprint) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;

    std::fseek(f, 0, SEEK_END);
    const long file_size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);

    if (file_size < (long)HEADER_SIZE) { std::fclose(f); return false; }

    // Read header fields.
    uint32_t magic = 0;
    uint64_t fingerprint = 0;
    float    origin[3] = {};
    float    cell = 0.0f;
    int32_t  nx = 0, ny = 0, nz = 0;
    uint64_t content_hash = 0;

    bool ok = true;
    auto get = [&](void* p, size_t n) {
        ok = ok && (std::fread(p, 1, n, f) == n);
    };

    get(&magic,        sizeof(magic));
    get(&fingerprint,  sizeof(fingerprint));
    get(origin,        sizeof(origin));
    get(&cell,         sizeof(cell));
    get(&nx,           sizeof(nx));
    get(&ny,           sizeof(ny));
    get(&nz,           sizeof(nz));
    get(&content_hash, sizeof(content_hash));

    if (!ok)                       { std::fclose(f); return false; }
    if (magic != PRB1_MAGIC)       { std::fclose(f); return false; }
    if (fingerprint != expected_fingerprint) { std::fclose(f); return false; }
    if (nx <= 0 || ny <= 0 || nz <= 0) { std::fclose(f); return false; }

    const size_t n_cells  = (size_t)nx * ny * nz;
    const size_t amb_bytes = n_cells * 4 * sizeof(float);
    const size_t dom_bytes = n_cells * 4 * sizeof(float);
    const long   expected_total = (long)(HEADER_SIZE + amb_bytes + dom_bytes);

    if (file_size != expected_total) { std::fclose(f); return false; }

    // Read blobs.
    out.grid.nx = nx; out.grid.ny = ny; out.grid.nz = nz;
    out.grid.cell = cell;
    out.grid.origin[0] = origin[0];
    out.grid.origin[1] = origin[1];
    out.grid.origin[2] = origin[2];
    out.ambient.resize(n_cells * 4);
    out.dominant.resize(n_cells * 4);

    get(out.ambient.data(),  amb_bytes);
    get(out.dominant.data(), dom_bytes);

    std::fclose(f);
    if (!ok) return false;

    // Verify content hash.
    std::vector<uint8_t> content_buf(amb_bytes + dom_bytes);
    std::memcpy(content_buf.data(),             out.ambient.data(),  amb_bytes);
    std::memcpy(content_buf.data() + amb_bytes, out.dominant.data(), dom_bytes);
    const uint64_t actual_hash = part_asset::fnv1a64(content_buf.data(), content_buf.size());
    if (actual_hash != content_hash) return false;

    return true;
}

} // namespace probe_volume
