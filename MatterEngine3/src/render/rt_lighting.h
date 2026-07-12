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

    // Task 5: shadow ray pipeline
    void resize(int screen_w, int screen_h);
    void prepare_depth(unsigned gl_depth_tex, int screen_w, int screen_h);
    bool trace_shadows(const float inv_vp[16], const float sun_dir[3]);
    void composite(int screen_w, int screen_h, float shadow_strength);
    unsigned shadow_texture() const { return shadow_gl_tex_; }

    // Phase 2: G-buffer
    bool begin_gbuffer(int screen_w, int screen_h);
    void end_gbuffer();
    unsigned gbuffer_fbo() const { return gbuffer_fbo_; }
    unsigned gbuffer_albedo_tex() const { return gbuffer_albedo_tex_; }
    unsigned gbuffer_normal_tex() const { return gbuffer_normal_tex_; }
    unsigned gbuffer_orm_tex() const { return gbuffer_orm_tex_; }
    unsigned gbuffer_depth_tex() const { return gbuffer_depth_tex_; }

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

    // GL resources (Task 5)
    unsigned depth_linear_tex_ = 0;
    unsigned shadow_gl_tex_    = 0;
    unsigned depth_lin_prog_   = 0;
    unsigned composite_prog_   = 0;
    unsigned scene_copy_tex_   = 0;
    unsigned scene_fbo_        = 0;
    unsigned dummy_vao_        = 0;
    int      trace_w_ = 0, trace_h_ = 0;
    int      screen_w_ = 0, screen_h_ = 0;

    // Phase 2: G-buffer FBO and textures (full resolution)
    unsigned gbuffer_fbo_         = 0;
    unsigned gbuffer_albedo_tex_  = 0;  // RGBA8: albedo.rgb, emission
    unsigned gbuffer_normal_tex_  = 0;  // RGBA16F: normal.xyz, translucency
    unsigned gbuffer_orm_tex_     = 0;  // RGBA8: roughness, metallic, bakedAO, materialId/255
    unsigned gbuffer_depth_tex_   = 0;  // DEPTH_COMPONENT32F
    int      gbuffer_w_ = 0, gbuffer_h_ = 0;

    void resize_gbuffer(int w, int h);

    // CUDA interop (Task 5)
    uint64_t cuda_depth_resource_  = 0;   // CUgraphicsResource
    uint64_t cuda_shadow_resource_ = 0;   // CUgraphicsResource
    bool interop_registered_ = false;

    // OptiX pipeline (Task 5)
    void*    pipeline_       = nullptr;   // OptixPipeline
    void*    raygen_pg_      = nullptr;   // OptixProgramGroup
    void*    miss_pg_        = nullptr;   // OptixProgramGroup
    void*    hit_pg_         = nullptr;   // OptixProgramGroup
    uint64_t sbt_raygen_buf_ = 0;         // CUdeviceptr
    uint64_t sbt_miss_buf_   = 0;         // CUdeviceptr
    uint64_t sbt_hit_buf_    = 0;         // CUdeviceptr
    uint64_t d_params_       = 0;         // CUdeviceptr

    bool build_pipeline(std::string& err);
    bool compile_gl_shaders(std::string& err);
};

} // namespace viewer

#else // !MATTER_HAVE_OPTIX

#include <cstdint>
#include <string>

namespace viewer {
class RtLighting {
public:
    bool init(std::string& err) { err = "not compiled with OptiX (rebuild with HAVE_CUDA=1)"; return false; }
    void shutdown() {}
    bool available() const { return false; }
    void register_part(uint64_t, const float*, int) {}
    void unregister_part(uint64_t) {}
    struct InstanceInput { uint64_t part_hash; int lod_level; float transform[16]; };
    void update_instances(const InstanceInput*, int) {}
    uint64_t tlas_handle() const { return 0; }
    void resize(int, int) {}
    void prepare_depth(unsigned, int, int) {}
    bool trace_shadows(const float[16], const float[3]) { return false; }
    void composite(int, int, float) {}
    unsigned shadow_texture() const { return 0; }
    bool begin_gbuffer(int, int) { return false; }
    void end_gbuffer() {}
    unsigned gbuffer_fbo() const { return 0; }
    unsigned gbuffer_albedo_tex() const { return 0; }
    unsigned gbuffer_normal_tex() const { return 0; }
    unsigned gbuffer_orm_tex() const { return 0; }
    unsigned gbuffer_depth_tex() const { return 0; }
};
} // namespace viewer

#endif // MATTER_HAVE_OPTIX
#endif // MATTER_RT_LIGHTING_H
