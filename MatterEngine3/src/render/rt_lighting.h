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

private:
    bool available_ = false;
    // CUDA device + OptiX context handles stored as void* to avoid
    // CUDA/OptiX headers leaking into every TU that includes this header.
    void* cu_ctx_     = nullptr;  // CUcontext
    void* optix_ctx_  = nullptr;  // OptixDeviceContext

    std::unordered_map<uint64_t, RtBlas> blas_cache_;
};

} // namespace viewer

#else // !MATTER_HAVE_OPTIX

#include <string>

namespace viewer {
class RtLighting {
public:
    bool init(std::string&) { return false; }
    void shutdown() {}
    bool available() const { return false; }
    void register_part(uint64_t, const float*, int) {}
    void unregister_part(uint64_t) {}
};
} // namespace viewer

#endif // MATTER_HAVE_OPTIX
#endif // MATTER_RT_LIGHTING_H
