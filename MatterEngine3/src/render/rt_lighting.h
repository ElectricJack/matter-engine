#ifndef MATTER_RT_LIGHTING_H
#define MATTER_RT_LIGHTING_H

#ifdef MATTER_HAVE_OPTIX

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace viewer {

struct RtBlas {
    uint64_t d_vertices = 0;   // CUdeviceptr — GPU vertex buffer
    uint64_t gas_buffer = 0;   // CUdeviceptr — compacted GAS buffer
    uint64_t traversable = 0;  // OptixTraversableHandle
    int vertex_count = 0;
};

class RtLighting {
public:
    RtLighting() = default;
    ~RtLighting();

    bool init(std::string& err);
    void shutdown();
    bool available() const { return available_; }

    void register_part(uint64_t part_hash, const float* vertices, int vertex_count);
    void unregister_part(uint64_t part_hash);

    struct InstanceInput {
        uint64_t part_hash;
        int      lod_level;
        float    transform[16];  // row-major 4x4
    };
    void update_instances(const InstanceInput* instances, int count);
    uint64_t tlas_handle() const { return tlas_handle_; }

private:
    bool available_ = false;
    // CUDA device + OptiX context handles stored as void* to avoid
    // CUDA/OptiX headers leaking into every TU that includes this header.
    void* cu_ctx_     = nullptr;  // CUcontext
    void* optix_ctx_  = nullptr;  // OptixDeviceContext

    std::unordered_map<uint64_t, RtBlas> blas_cache_;

    uint64_t tlas_buffer_   = 0;   // CUdeviceptr — TLAS output buffer
    size_t   tlas_buf_size_ = 0;
    uint64_t tlas_handle_   = 0;   // OptixTraversableHandle
    uint64_t d_instances_   = 0;   // CUdeviceptr — OptixInstance array on device
    int      instance_cap_  = 0;
};

} // namespace viewer

#else // !MATTER_HAVE_OPTIX

#include <cstdint>
#include <string>

namespace viewer {
class RtLighting {
public:
    bool init(std::string&) { return false; }
    void shutdown() {}
    bool available() const { return false; }
    void register_part(uint64_t, const float*, int) {}
    void unregister_part(uint64_t) {}
    struct InstanceInput { uint64_t part_hash; int lod_level; float transform[16]; };
    void update_instances(const InstanceInput*, int) {}
    uint64_t tlas_handle() const { return 0; }
};
} // namespace viewer

#endif // MATTER_HAVE_OPTIX
#endif // MATTER_RT_LIGHTING_H
