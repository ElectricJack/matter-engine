#include "rt_lighting.h"

#ifdef MATTER_HAVE_OPTIX

// Prevent wingdi.h / winuser.h symbols (Rectangle, CloseWindow, ShowCursor)
// from colliding with raylib's identically-named declarations.
#ifdef _WIN32
#define NOGDI
#define NOUSER
#endif

// Raylib must come before glad to avoid double-definition of GL types.
// glad.h must precede cudaGL.h which includes <GL/gl.h>.
#include "raylib.h"
#include "external/glad.h"

#include <cuda.h>
#include <cudaGL.h>
#include <optix.h>
#include <optix_stubs.h>
#include <optix_function_table_definition.h>

#include "shaders_rt/rt_params.h"          // RtLaunchParams (shared host/device struct)
#include "../../shaders_gen/embedded_rt_shaders.h"
#include "shader_source.h"   // matter::shader_text

#include <cstdio>
#include <cstring>
#include <vector>

namespace viewer {

static void optix_log_callback(unsigned int level, const char* tag,
                               const char* msg, void*) {
    fprintf(stderr, "[OptiX][%u][%s]: %s\n", level, tag, msg);
}

bool RtLighting::init(std::string& err) {
    // 1. Initialize CUDA driver API.
    CUresult cu_res = cuInit(0);
    if (cu_res != CUDA_SUCCESS) {
        err = "cuInit failed: " + std::to_string((int)cu_res);
        return false;
    }

    // 2. Get device 0 and create/retain primary context.
    CUdevice device = 0;
    cu_res = cuDeviceGet(&device, 0);
    if (cu_res != CUDA_SUCCESS) {
        err = "cuDeviceGet failed";
        return false;
    }
    CUcontext ctx = nullptr;
    cu_res = cuDevicePrimaryCtxRetain(&ctx, device);
    if (cu_res != CUDA_SUCCESS) {
        err = "cuDevicePrimaryCtxRetain failed";
        return false;
    }
    cuCtxSetCurrent(ctx);
    cu_ctx_ = (void*)ctx;

    // 3. Initialize OptiX.
    OptixResult opt_res = optixInit();
    if (opt_res != OPTIX_SUCCESS) {
        err = "optixInit failed: " + std::to_string((int)opt_res);
        return false;
    }

    // 4. Create OptiX device context.
    OptixDeviceContextOptions ctx_opts = {};
    ctx_opts.logCallbackFunction = optix_log_callback;
    ctx_opts.logCallbackLevel    = 4;  // warnings + errors
    OptixDeviceContext optix_context = nullptr;
    opt_res = optixDeviceContextCreate(ctx, &ctx_opts, &optix_context);
    if (opt_res != OPTIX_SUCCESS) {
        err = "optixDeviceContextCreate failed: " + std::to_string((int)opt_res);
        return false;
    }
    optix_ctx_ = (void*)optix_context;

    available_ = true;
    printf("RtLighting: OptiX %d.%d initialized on device 0\n",
           OPTIX_VERSION / 10000, (OPTIX_VERSION % 10000) / 100);

    // Task 5: build the shadow ray pipeline and compile GL shaders.
    if (!build_pipeline(err)) return false;
    if (!compile_gl_shaders(err)) return false;

    // Task 3: build the full lighting pipeline.
    if (!build_lighting_pipeline(err)) return false;

    // Task 5: set up OptiX denoiser scaffolding.
    if (!init_denoiser(err)) return false;

    return true;
}

void RtLighting::shutdown() {
    // Free pipeline resources.
    if (d_params_)       { cuMemFree((CUdeviceptr)d_params_);       d_params_ = 0; }
    if (sbt_raygen_buf_) { cuMemFree((CUdeviceptr)sbt_raygen_buf_); sbt_raygen_buf_ = 0; }
    if (sbt_miss_buf_)   { cuMemFree((CUdeviceptr)sbt_miss_buf_);   sbt_miss_buf_ = 0; }
    if (sbt_hit_buf_)    { cuMemFree((CUdeviceptr)sbt_hit_buf_);    sbt_hit_buf_ = 0; }
    if (pipeline_)       { optixPipelineDestroy((OptixPipeline)pipeline_);     pipeline_ = nullptr; }
    if (raygen_pg_)      { optixProgramGroupDestroy((OptixProgramGroup)raygen_pg_); raygen_pg_ = nullptr; }
    if (miss_pg_)        { optixProgramGroupDestroy((OptixProgramGroup)miss_pg_);   miss_pg_ = nullptr; }
    if (hit_pg_)         { optixProgramGroupDestroy((OptixProgramGroup)hit_pg_);    hit_pg_ = nullptr; }

    // Free CUDA interop.
    if (interop_registered_) {
        cuGraphicsUnregisterResource((CUgraphicsResource)cuda_depth_resource_);
        cuGraphicsUnregisterResource((CUgraphicsResource)cuda_shadow_resource_);
        interop_registered_ = false;
        cuda_depth_resource_ = 0;
        cuda_shadow_resource_ = 0;
    }

    // Free GL resources.
    if (depth_linear_tex_) { glDeleteTextures(1, &depth_linear_tex_); depth_linear_tex_ = 0; }
    if (shadow_gl_tex_)    { glDeleteTextures(1, &shadow_gl_tex_);    shadow_gl_tex_ = 0; }
    if (scene_copy_tex_)   { glDeleteTextures(1, &scene_copy_tex_);   scene_copy_tex_ = 0; }
    if (scene_fbo_)        { glDeleteFramebuffers(1, &scene_fbo_);    scene_fbo_ = 0; }
    if (dummy_vao_)        { glDeleteVertexArrays(1, &dummy_vao_);    dummy_vao_ = 0; }
    if (depth_lin_prog_)           { glDeleteProgram(depth_lin_prog_);            depth_lin_prog_ = 0; }
    if (composite_prog_)           { glDeleteProgram(composite_prog_);            composite_prog_ = 0; }
    if (lighting_composite_prog_)  { glDeleteProgram(lighting_composite_prog_);   lighting_composite_prog_ = 0; }

    // Phase 2 Task 5: denoiser and accumulation buffer cleanup.
    if (denoiser_)          { optixDenoiserDestroy((OptixDenoiser)denoiser_);      denoiser_ = nullptr; }
    if (d_denoiser_state_)  { cuMemFree((CUdeviceptr)d_denoiser_state_);  d_denoiser_state_ = 0; }
    if (d_denoiser_scratch_){ cuMemFree((CUdeviceptr)d_denoiser_scratch_);d_denoiser_scratch_ = 0; }
    if (d_denoised_buffer_) { cuMemFree((CUdeviceptr)d_denoised_buffer_); d_denoised_buffer_ = 0; }
    if (d_denoise_albedo_)  { cuMemFree((CUdeviceptr)d_denoise_albedo_);  d_denoise_albedo_ = 0; }
    if (d_denoise_normal_)  { cuMemFree((CUdeviceptr)d_denoise_normal_);  d_denoise_normal_ = 0; }
    if (d_accum_buffer_)    { cuMemFree((CUdeviceptr)d_accum_buffer_);    d_accum_buffer_ = 0; }
    if (d_current_buffer_)  { cuMemFree((CUdeviceptr)d_current_buffer_);  d_current_buffer_ = 0; }
    denoiser_state_size_ = denoiser_scratch_size_ = 0;
    accum_w_ = accum_h_ = 0;
    frame_index_ = 0;

    // Phase 2 Task 3: lighting pipeline cleanup.
    if (lighting_pipeline_)    { optixPipelineDestroy((OptixPipeline)lighting_pipeline_);          lighting_pipeline_ = nullptr; }
    if (lighting_raygen_pg_)   { optixProgramGroupDestroy((OptixProgramGroup)lighting_raygen_pg_); lighting_raygen_pg_ = nullptr; }
    if (radiance_miss_pg_)     { optixProgramGroupDestroy((OptixProgramGroup)radiance_miss_pg_);   radiance_miss_pg_ = nullptr; }
    if (closesthit_pg_)        { optixProgramGroupDestroy((OptixProgramGroup)closesthit_pg_);       closesthit_pg_ = nullptr; }
    if (sbt_lighting_raygen_)  { cuMemFree((CUdeviceptr)sbt_lighting_raygen_); sbt_lighting_raygen_ = 0; }
    if (sbt_lighting_miss_)    { cuMemFree((CUdeviceptr)sbt_lighting_miss_);   sbt_lighting_miss_ = 0; }
    if (lighting_interop_registered_) {
        cuGraphicsUnregisterResource((CUgraphicsResource)cuda_lighting_resource_);
        cuGraphicsUnregisterResource((CUgraphicsResource)cuda_gb_albedo_resource_);
        cuGraphicsUnregisterResource((CUgraphicsResource)cuda_gb_normal_resource_);
        cuGraphicsUnregisterResource((CUgraphicsResource)cuda_gb_orm_resource_);
        lighting_interop_registered_ = false;
    }
    if (lighting_gl_tex_)  { glDeleteTextures(1, &lighting_gl_tex_);  lighting_gl_tex_ = 0; }

    // Phase 2: G-buffer cleanup.
    if (gbuffer_fbo_)        { glDeleteFramebuffers(1, &gbuffer_fbo_);          gbuffer_fbo_ = 0; }
    if (gbuffer_albedo_tex_) { glDeleteTextures(1, &gbuffer_albedo_tex_);        gbuffer_albedo_tex_ = 0; }
    if (gbuffer_normal_tex_) { glDeleteTextures(1, &gbuffer_normal_tex_);        gbuffer_normal_tex_ = 0; }
    if (gbuffer_orm_tex_)    { glDeleteTextures(1, &gbuffer_orm_tex_);           gbuffer_orm_tex_ = 0; }
    if (gbuffer_depth_tex_)  { glDeleteTextures(1, &gbuffer_depth_tex_);         gbuffer_depth_tex_ = 0; }
    gbuffer_w_ = gbuffer_h_ = 0;

    // Phase 2: free material table and per-BLAS SBT.
    if (d_material_table_) { cuMemFree((CUdeviceptr)d_material_table_); d_material_table_ = 0; }
    if (d_hitgroup_sbt_)   { cuMemFree((CUdeviceptr)d_hitgroup_sbt_);   d_hitgroup_sbt_ = 0; }
    hitgroup_sbt_cap_    = 0;
    next_blas_sbt_index_ = 0;
    material_count_      = 0;

    // Free all BLAS resources before destroying the OptiX context.
    for (auto& [_, blas] : blas_cache_) {
        cuMemFree((CUdeviceptr)blas.d_vertices);
        cuMemFree((CUdeviceptr)blas.d_normals);
        cuMemFree((CUdeviceptr)blas.d_texcoords);
        cuMemFree((CUdeviceptr)blas.gas_buffer);
    }
    blas_cache_.clear();

    // Free TLAS resources.
    if (tlas_buffer_) { cuMemFree((CUdeviceptr)tlas_buffer_); tlas_buffer_ = 0; }
    if (d_instances_) { cuMemFree((CUdeviceptr)d_instances_); d_instances_ = 0; }
    tlas_buf_size_ = 0;
    tlas_handle_   = 0;
    instance_cap_  = 0;

    if (optix_ctx_) {
        optixDeviceContextDestroy((OptixDeviceContext)optix_ctx_);
        optix_ctx_ = nullptr;
    }
    if (cu_ctx_) {
        CUdevice device = 0;
        cuDeviceGet(&device, 0);
        cuDevicePrimaryCtxRelease(device);
        cu_ctx_ = nullptr;
    }
    available_ = false;
}

void RtLighting::register_part(uint64_t part_hash, const float* vertices,
                                const float* normals, const float* texcoords,
                                int vertex_count) {
    if (!available_ || blas_cache_.count(part_hash)) return;

    RtBlas blas;
    blas.vertex_count = vertex_count;

    // Upload vertex positions (for BLAS geometry).
    size_t verts_bytes = (size_t)vertex_count * 3 * sizeof(float);
    CUdeviceptr d_verts = 0;
    cuMemAlloc(&d_verts, verts_bytes);
    cuMemcpyHtoD(d_verts, vertices, verts_bytes);
    blas.d_vertices = (uint64_t)d_verts;

    // Upload normals (for closest-hit material queries).
    size_t norms_bytes = (size_t)vertex_count * 3 * sizeof(float);
    CUdeviceptr d_norms = 0;
    cuMemAlloc(&d_norms, norms_bytes);
    cuMemcpyHtoD(d_norms, normals, norms_bytes);
    blas.d_normals = (uint64_t)d_norms;

    // Upload texcoords (materialId, bakedAO per vertex).
    size_t tc_bytes = (size_t)vertex_count * 2 * sizeof(float);
    CUdeviceptr d_tc = 0;
    cuMemAlloc(&d_tc, tc_bytes);
    cuMemcpyHtoD(d_tc, texcoords, tc_bytes);
    blas.d_texcoords = (uint64_t)d_tc;

    OptixAccelBuildOptions accel_opts = {};
    accel_opts.buildFlags = OPTIX_BUILD_FLAG_ALLOW_COMPACTION | OPTIX_BUILD_FLAG_PREFER_FAST_TRACE;
    accel_opts.operation  = OPTIX_BUILD_OPERATION_BUILD;

    OptixBuildInput build_input = {};
    build_input.type = OPTIX_BUILD_INPUT_TYPE_TRIANGLES;
    auto& tri = build_input.triangleArray;
    tri.vertexFormat        = OPTIX_VERTEX_FORMAT_FLOAT3;
    tri.vertexStrideInBytes = 3 * sizeof(float);
    tri.numVertices         = (unsigned int)vertex_count;
    CUdeviceptr vertex_ptrs[1] = { d_verts };
    tri.vertexBuffers       = vertex_ptrs;
    tri.numSbtRecords       = 1;
    unsigned int flags[1]   = { OPTIX_GEOMETRY_FLAG_NONE };
    tri.flags               = flags;

    OptixAccelBufferSizes buf_sizes = {};
    optixAccelComputeMemoryUsage((OptixDeviceContext)optix_ctx_, &accel_opts,
                                 &build_input, 1, &buf_sizes);

    CUdeviceptr d_temp = 0, d_output = 0;
    cuMemAlloc(&d_temp, buf_sizes.tempSizeInBytes);
    cuMemAlloc(&d_output, buf_sizes.outputSizeInBytes);

    CUdeviceptr d_compacted_size = 0;
    cuMemAlloc(&d_compacted_size, sizeof(uint64_t));
    OptixAccelEmitDesc emit_desc = {};
    emit_desc.type   = OPTIX_PROPERTY_TYPE_COMPACTED_SIZE;
    emit_desc.result = d_compacted_size;

    OptixTraversableHandle handle = 0;
    optixAccelBuild((OptixDeviceContext)optix_ctx_, 0,
                    &accel_opts, &build_input, 1,
                    d_temp, buf_sizes.tempSizeInBytes,
                    d_output, buf_sizes.outputSizeInBytes,
                    &handle, &emit_desc, 1);
    cuCtxSynchronize();
    cuMemFree(d_temp);

    uint64_t compacted_size = 0;
    cuMemcpyDtoH(&compacted_size, d_compacted_size, sizeof(uint64_t));
    cuMemFree(d_compacted_size);

    CUdeviceptr d_compacted = 0;
    cuMemAlloc(&d_compacted, compacted_size);
    optixAccelCompact((OptixDeviceContext)optix_ctx_, 0, handle,
                      d_compacted, compacted_size, &handle);
    cuCtxSynchronize();
    cuMemFree(d_output);

    blas.gas_buffer      = (uint64_t)d_compacted;
    blas.traversable     = (uint64_t)handle;
    blas.blas_sbt_index  = next_blas_sbt_index_++;
    sbt_dirty_           = true;
    blas_cache_[part_hash] = blas;
}

void RtLighting::unregister_part(uint64_t part_hash) {
    auto it = blas_cache_.find(part_hash);
    if (it == blas_cache_.end()) return;
    cuMemFree((CUdeviceptr)it->second.d_vertices);
    cuMemFree((CUdeviceptr)it->second.d_normals);
    cuMemFree((CUdeviceptr)it->second.d_texcoords);
    cuMemFree((CUdeviceptr)it->second.gas_buffer);
    blas_cache_.erase(it);
    sbt_dirty_ = true;
}

void RtLighting::rebuild_hitgroup_sbt() {
    if (!sbt_dirty_) return;
    sbt_dirty_ = false;

    int num_blas = next_blas_sbt_index_;
    if (num_blas == 0) return;

    struct alignas(OPTIX_SBT_RECORD_ALIGNMENT) HitGroupRecord {
        char header[OPTIX_SBT_RECORD_HEADER_SIZE];
        HitGroupData data;
    };

    int num_records = num_blas * 2;  // 2 ray types per BLAS
    std::vector<HitGroupRecord> records((size_t)num_records);

    for (auto& [hash, blas] : blas_cache_) {
        int base = blas.blas_sbt_index * 2;

        // Ray type 0: shadow (anyhit — no per-geometry data needed but struct must be filled)
        {
            auto& rec = records[(size_t)base + 0];
            memset(&rec, 0, sizeof(rec));
            optixSbtRecordPackHeader((OptixProgramGroup)hit_pg_, &rec);
            rec.data.normals      = nullptr;
            rec.data.texcoords    = nullptr;
            rec.data.vertex_count = 0;
        }

        // Ray type 1: radiance (closest-hit — needs per-geometry attribute data)
        {
            auto& rec = records[(size_t)base + 1];
            memset(&rec, 0, sizeof(rec));
            // closesthit_pg_ is built in Task 3. Until then, use hit_pg_ as placeholder.
            optixSbtRecordPackHeader(
                (OptixProgramGroup)(closesthit_pg_ ? closesthit_pg_ : hit_pg_), &rec);
            rec.data.normals      = reinterpret_cast<float*>((CUdeviceptr)blas.d_normals);
            rec.data.texcoords    = reinterpret_cast<float*>((CUdeviceptr)blas.d_texcoords);
            rec.data.vertex_count = blas.vertex_count;
        }
    }

    size_t total_bytes = (size_t)num_records * sizeof(HitGroupRecord);
    if (num_records > hitgroup_sbt_cap_) {
        if (d_hitgroup_sbt_) cuMemFree((CUdeviceptr)d_hitgroup_sbt_);
        CUdeviceptr d = 0;
        cuMemAlloc(&d, total_bytes);
        d_hitgroup_sbt_    = (uint64_t)d;
        hitgroup_sbt_cap_  = num_records;
    }
    cuMemcpyHtoD((CUdeviceptr)d_hitgroup_sbt_, records.data(), total_bytes);
}

void RtLighting::upload_material_table(const float* table, int count) {
    if (!available_) return;
    size_t bytes = (size_t)count * 12 * sizeof(float);
    if (d_material_table_ == 0 || count != material_count_) {
        if (d_material_table_) cuMemFree((CUdeviceptr)d_material_table_);
        CUdeviceptr d = 0;
        cuMemAlloc(&d, bytes);
        d_material_table_ = (uint64_t)d;
    }
    cuMemcpyHtoD((CUdeviceptr)d_material_table_, table, bytes);
    material_count_ = count;
}

void RtLighting::update_instances(const InstanceInput* instances, int count) {
    if (!available_ || count == 0) return;
    rebuild_hitgroup_sbt();

    std::vector<OptixInstance> optix_instances;
    optix_instances.reserve(count);

    for (int i = 0; i < count; ++i) {
        auto it = blas_cache_.find(instances[i].part_hash);
        if (it == blas_cache_.end()) continue;

        OptixInstance inst = {};
        const float* t = instances[i].transform;
        inst.transform[0]  = t[0];  inst.transform[1]  = t[1];  inst.transform[2]  = t[2];  inst.transform[3]  = t[3];
        inst.transform[4]  = t[4];  inst.transform[5]  = t[5];  inst.transform[6]  = t[6];  inst.transform[7]  = t[7];
        inst.transform[8]  = t[8];  inst.transform[9]  = t[9];  inst.transform[10] = t[10]; inst.transform[11] = t[11];

        inst.instanceId        = i;
        inst.sbtOffset         = (unsigned int)(it->second.blas_sbt_index * 2);  // 2 ray types per BLAS
        inst.visibilityMask    = 0xFF;
        inst.flags             = OPTIX_INSTANCE_FLAG_NONE;
        inst.traversableHandle = (OptixTraversableHandle)it->second.traversable;
        optix_instances.push_back(inst);
    }

    if (optix_instances.empty()) return;
    int inst_count = (int)optix_instances.size();

    size_t inst_bytes = inst_count * sizeof(OptixInstance);
    if (inst_count > instance_cap_) {
        if (d_instances_) cuMemFree((CUdeviceptr)d_instances_);
        CUdeviceptr d_inst = 0;
        cuMemAlloc(&d_inst, inst_bytes);
        d_instances_ = (uint64_t)d_inst;
        instance_cap_ = inst_count;
    }
    cuMemcpyHtoD((CUdeviceptr)d_instances_, optix_instances.data(), inst_bytes);

    OptixBuildInput build_input = {};
    build_input.type = OPTIX_BUILD_INPUT_TYPE_INSTANCES;
    build_input.instanceArray.instances    = (CUdeviceptr)d_instances_;
    build_input.instanceArray.numInstances = inst_count;

    OptixAccelBuildOptions accel_opts = {};
    accel_opts.buildFlags = OPTIX_BUILD_FLAG_ALLOW_UPDATE | OPTIX_BUILD_FLAG_PREFER_FAST_TRACE;
    accel_opts.operation  = OPTIX_BUILD_OPERATION_BUILD;

    OptixAccelBufferSizes buf_sizes = {};
    optixAccelComputeMemoryUsage((OptixDeviceContext)optix_ctx_, &accel_opts,
                                 &build_input, 1, &buf_sizes);

    if (buf_sizes.outputSizeInBytes > tlas_buf_size_) {
        if (tlas_buffer_) cuMemFree((CUdeviceptr)tlas_buffer_);
        CUdeviceptr d_tlas = 0;
        cuMemAlloc(&d_tlas, buf_sizes.outputSizeInBytes);
        tlas_buffer_   = (uint64_t)d_tlas;
        tlas_buf_size_ = buf_sizes.outputSizeInBytes;
    }

    CUdeviceptr d_temp = 0;
    cuMemAlloc(&d_temp, buf_sizes.tempSizeInBytes);

    OptixTraversableHandle handle = 0;
    optixAccelBuild((OptixDeviceContext)optix_ctx_, 0,
                    &accel_opts, &build_input, 1,
                    d_temp, buf_sizes.tempSizeInBytes,
                    (CUdeviceptr)tlas_buffer_, buf_sizes.outputSizeInBytes,
                    &handle, nullptr, 0);
    cuCtxSynchronize();
    cuMemFree(d_temp);

    tlas_handle_ = (uint64_t)handle;
}

// ---------------------------------------------------------------------------
// Task 5: OptiX pipeline construction
// ---------------------------------------------------------------------------
bool RtLighting::build_pipeline(std::string& err) {
    auto ctx = (OptixDeviceContext)optix_ctx_;

    OptixModuleCompileOptions mod_opts = {};
    mod_opts.maxRegisterCount = OPTIX_COMPILE_DEFAULT_MAX_REGISTER_COUNT;
    mod_opts.optLevel         = OPTIX_COMPILE_OPTIMIZATION_DEFAULT;

    OptixPipelineCompileOptions pipe_opts = {};
    pipe_opts.usesMotionBlur                   = false;
    pipe_opts.traversableGraphFlags            = OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_ANY;
    pipe_opts.numPayloadValues                 = 1;
    pipe_opts.numAttributeValues               = 2;
    pipe_opts.exceptionFlags                   = OPTIX_EXCEPTION_FLAG_NONE;
    pipe_opts.pipelineLaunchParamsVariableName  = "params";

    char log[2048]; size_t log_size;

    auto create_module = [&](const char* ptx_str, OptixModule& mod) -> bool {
        log_size = sizeof(log);
        OptixResult r = optixModuleCreate(ctx, &mod_opts, &pipe_opts,
                                          ptx_str, strlen(ptx_str),
                                          log, &log_size, &mod);
        if (r != OPTIX_SUCCESS) {
            err = std::string("optixModuleCreate: ") + log;
            return false;
        }
        return true;
    };

    OptixModule mod_raygen = nullptr, mod_miss = nullptr, mod_hit = nullptr;
    if (!create_module(matter_rt_embedded::shadow_raygen_ptx, mod_raygen)) return false;
    if (!create_module(matter_rt_embedded::shadow_miss_ptx, mod_miss)) return false;
    if (!create_module(matter_rt_embedded::shadow_hit_ptx, mod_hit)) return false;

    OptixProgramGroupOptions pg_opts = {};
    OptixProgramGroupDesc pg_desc = {};

    pg_desc.kind = OPTIX_PROGRAM_GROUP_KIND_RAYGEN;
    pg_desc.raygen.module = mod_raygen;
    pg_desc.raygen.entryFunctionName = "__raygen__shadow";
    log_size = sizeof(log);
    optixProgramGroupCreate(ctx, &pg_desc, 1, &pg_opts, log, &log_size,
                            (OptixProgramGroup*)&raygen_pg_);

    pg_desc = {};
    pg_desc.kind = OPTIX_PROGRAM_GROUP_KIND_MISS;
    pg_desc.miss.module = mod_miss;
    pg_desc.miss.entryFunctionName = "__miss__shadow";
    log_size = sizeof(log);
    optixProgramGroupCreate(ctx, &pg_desc, 1, &pg_opts, log, &log_size,
                            (OptixProgramGroup*)&miss_pg_);

    pg_desc = {};
    pg_desc.kind = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
    pg_desc.hitgroup.moduleAH            = mod_hit;
    pg_desc.hitgroup.entryFunctionNameAH = "__anyhit__shadow";
    log_size = sizeof(log);
    optixProgramGroupCreate(ctx, &pg_desc, 1, &pg_opts, log, &log_size,
                            (OptixProgramGroup*)&hit_pg_);

    OptixProgramGroup groups[] = {
        (OptixProgramGroup)raygen_pg_,
        (OptixProgramGroup)miss_pg_,
        (OptixProgramGroup)hit_pg_
    };
    OptixPipelineLinkOptions link_opts = {};
    link_opts.maxTraceDepth = 1;
    log_size = sizeof(log);
    optixPipelineCreate(ctx, &pipe_opts, &link_opts,
                        groups, 3, log, &log_size,
                        (OptixPipeline*)&pipeline_);

    struct alignas(OPTIX_SBT_RECORD_ALIGNMENT) SbtRecord {
        char header[OPTIX_SBT_RECORD_HEADER_SIZE];
    };

    auto upload_sbt = [](void* pg, uint64_t& buf) {
        SbtRecord rec = {};
        optixSbtRecordPackHeader((OptixProgramGroup)pg, &rec);
        CUdeviceptr d_buf = 0;
        cuMemAlloc(&d_buf, sizeof(SbtRecord));
        cuMemcpyHtoD(d_buf, &rec, sizeof(SbtRecord));
        buf = (uint64_t)d_buf;
    };
    upload_sbt(raygen_pg_, sbt_raygen_buf_);
    upload_sbt(miss_pg_,   sbt_miss_buf_);
    upload_sbt(hit_pg_,    sbt_hit_buf_);

    CUdeviceptr d_p = 0;
    cuMemAlloc(&d_p, sizeof(RtLaunchParams));
    d_params_ = (uint64_t)d_p;
    return true;
}

// ---------------------------------------------------------------------------
// Task 5: GL shader compilation (depth linearize + composite)
// ---------------------------------------------------------------------------
bool RtLighting::compile_gl_shaders(std::string& err) {
    auto compile_shader = [](GLenum type, const char* src) -> unsigned {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) { glDeleteShader(s); return 0; }
        return s;
    };

    auto link_program = [](unsigned vs, unsigned fs) -> unsigned {
        GLuint p = glCreateProgram();
        glAttachShader(p, vs);
        glAttachShader(p, fs);
        glLinkProgram(p);
        glDeleteShader(vs);
        glDeleteShader(fs);
        return p;
    };

    // Depth linearize compute shader.
    {
        std::string src, serr;
        if (!matter::shader_text("shaders_gpu/depth_linearize.comp", src, serr)) {
            err = "depth_linearize.comp not found in embedded shaders: " + serr;
            return false;
        }
        GLuint cs = compile_shader(GL_COMPUTE_SHADER, src.c_str());
        if (!cs) { err = "depth_linearize.comp compile failed"; return false; }
        depth_lin_prog_ = glCreateProgram();
        glAttachShader(depth_lin_prog_, cs);
        glLinkProgram(depth_lin_prog_);
        glDeleteShader(cs);
    }

    // Load the fullscreen vertex shader once; reused for all composite passes.
    std::string vs_src, vs_err;
    if (!matter::shader_text("shaders_gpu/rt_composite.vs", vs_src, vs_err)) {
        err = "rt_composite.vs not found: " + vs_err;
        return false;
    }

    // Phase 1: shadow composite shader.
    {
        std::string fs_src, fs_err;
        if (!matter::shader_text("shaders_gpu/rt_composite.fs", fs_src, fs_err)) {
            err = "rt_composite.fs not found: " + fs_err;
            return false;
        }
        GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src.c_str());
        GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src.c_str());
        if (!vs || !fs) { err = "rt_composite compile failed"; return false; }
        composite_prog_ = link_program(vs, fs);
    }

    // Phase 2: lighting composite shader (same fullscreen VS + new FS).
    {
        std::string fs_src2, fs_err2;
        if (!matter::shader_text("shaders_gpu/rt_lighting_composite.fs", fs_src2, fs_err2)) {
            err = "rt_lighting_composite.fs not found: " + fs_err2;
            return false;
        }
        GLuint vs2 = compile_shader(GL_VERTEX_SHADER, vs_src.c_str());  // reuse vs_src from above
        GLuint fs2 = compile_shader(GL_FRAGMENT_SHADER, fs_src2.c_str());
        if (!vs2 || !fs2) { err = "rt_lighting_composite compile failed"; return false; }
        lighting_composite_prog_ = link_program(vs2, fs2);
    }

    glGenVertexArrays(1, &dummy_vao_);
    return true;
}

// ---------------------------------------------------------------------------
// Task 5: resize / prepare_depth / trace_shadows / composite
// ---------------------------------------------------------------------------
void RtLighting::resize(int screen_w, int screen_h) {
    int new_tw = screen_w / 2;
    int new_th = screen_h / 2;
    if (new_tw < 1) new_tw = 1;
    if (new_th < 1) new_th = 1;
    if (new_tw == trace_w_ && new_th == trace_h_ && screen_w == screen_w_) return;
    trace_w_ = new_tw;
    trace_h_ = new_th;
    screen_w_ = screen_w;
    screen_h_ = screen_h;

    if (interop_registered_) {
        cuGraphicsUnregisterResource((CUgraphicsResource)cuda_depth_resource_);
        cuGraphicsUnregisterResource((CUgraphicsResource)cuda_shadow_resource_);
        interop_registered_ = false;
    }

    if (depth_linear_tex_) glDeleteTextures(1, &depth_linear_tex_);
    glGenTextures(1, &depth_linear_tex_);
    glBindTexture(GL_TEXTURE_2D, depth_linear_tex_);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32F, trace_w_, trace_h_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    if (shadow_gl_tex_) glDeleteTextures(1, &shadow_gl_tex_);
    glGenTextures(1, &shadow_gl_tex_);
    glBindTexture(GL_TEXTURE_2D, shadow_gl_tex_);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32F, trace_w_, trace_h_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);

    CUgraphicsResource depth_res = nullptr, shadow_res = nullptr;
    cuGraphicsGLRegisterImage(&depth_res, depth_linear_tex_,
                              GL_TEXTURE_2D, CU_GRAPHICS_REGISTER_FLAGS_READ_ONLY);
    cuGraphicsGLRegisterImage(&shadow_res, shadow_gl_tex_,
                              GL_TEXTURE_2D, CU_GRAPHICS_REGISTER_FLAGS_SURFACE_LDST);
    cuda_depth_resource_  = (uint64_t)depth_res;
    cuda_shadow_resource_ = (uint64_t)shadow_res;
    interop_registered_ = true;

    if (scene_copy_tex_) glDeleteTextures(1, &scene_copy_tex_);
    glGenTextures(1, &scene_copy_tex_);
    glBindTexture(GL_TEXTURE_2D, scene_copy_tex_);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, screen_w, screen_h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);

    if (scene_fbo_) glDeleteFramebuffers(1, &scene_fbo_);
    glGenFramebuffers(1, &scene_fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, scene_fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, scene_copy_tex_, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void RtLighting::prepare_depth(unsigned gl_depth_tex, int screen_w, int screen_h) {
    resize(screen_w, screen_h);

    glUseProgram(depth_lin_prog_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gl_depth_tex);
    glUniform1i(glGetUniformLocation(depth_lin_prog_, "u_depth"), 0);
    glBindImageTexture(0, depth_linear_tex_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);

    int gx = (trace_w_ + 15) / 16;
    int gy = (trace_h_ + 15) / 16;
    glDispatchCompute(gx, gy, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    glUseProgram(0);
}

bool RtLighting::trace_shadows(const float inv_vp[16], const float sun_dir[3]) {
    if (!available_ || !tlas_handle_ || trace_w_ == 0) return false;

    CUgraphicsResource resources[2] = {
        (CUgraphicsResource)cuda_depth_resource_,
        (CUgraphicsResource)cuda_shadow_resource_
    };
    cuGraphicsMapResources(2, resources, 0);

    CUarray depth_array = nullptr, shadow_array = nullptr;
    cuGraphicsSubResourceGetMappedArray(&depth_array,
        (CUgraphicsResource)cuda_depth_resource_, 0, 0);
    cuGraphicsSubResourceGetMappedArray(&shadow_array,
        (CUgraphicsResource)cuda_shadow_resource_, 0, 0);

    CUDA_RESOURCE_DESC res_desc = {};
    res_desc.resType = CU_RESOURCE_TYPE_ARRAY;

    res_desc.res.array.hArray = depth_array;
    CUsurfObject depth_surf = 0;
    cuSurfObjectCreate(&depth_surf, &res_desc);

    res_desc.res.array.hArray = shadow_array;
    CUsurfObject shadow_surf = 0;
    cuSurfObjectCreate(&shadow_surf, &res_desc);

    RtLaunchParams lp = {};
    lp.tlas           = (OptixTraversableHandle)tlas_handle_;
    memcpy(lp.inv_vp, inv_vp, 16 * sizeof(float));
    memcpy(lp.sun_dir, sun_dir, 3 * sizeof(float));
    lp.width          = trace_w_;
    lp.height         = trace_h_;
    lp.depth_surface  = (unsigned long long)depth_surf;
    lp.shadow_surface = (unsigned long long)shadow_surf;

    cuMemcpyHtoD((CUdeviceptr)d_params_, &lp, sizeof(RtLaunchParams));

    // Per-BLAS SBT: each BLAS gets 2 records (shadow ray type 0, radiance ray type 1).
    struct alignas(OPTIX_SBT_RECORD_ALIGNMENT) HitGroupRecord {
        char header[OPTIX_SBT_RECORD_HEADER_SIZE];
        HitGroupData data;
    };

    OptixShaderBindingTable sbt = {};
    sbt.raygenRecord                = (CUdeviceptr)sbt_raygen_buf_;
    sbt.missRecordBase              = (CUdeviceptr)sbt_miss_buf_;
    sbt.missRecordStrideInBytes     = OPTIX_SBT_RECORD_HEADER_SIZE;
    sbt.missRecordCount             = 1;
    if (d_hitgroup_sbt_ && next_blas_sbt_index_ > 0) {
        sbt.hitgroupRecordBase          = (CUdeviceptr)d_hitgroup_sbt_;
        sbt.hitgroupRecordStrideInBytes = sizeof(HitGroupRecord);
        sbt.hitgroupRecordCount         = (unsigned int)(next_blas_sbt_index_ * 2);
    } else {
        // Fallback: no BLAS registered yet, use the Phase 1 single record.
        sbt.hitgroupRecordBase          = (CUdeviceptr)sbt_hit_buf_;
        sbt.hitgroupRecordStrideInBytes = OPTIX_SBT_RECORD_HEADER_SIZE;
        sbt.hitgroupRecordCount         = 1;
    }

    optixLaunch((OptixPipeline)pipeline_, 0,
                (CUdeviceptr)d_params_, sizeof(RtLaunchParams),
                &sbt, trace_w_, trace_h_, 1);
    cuCtxSynchronize();

    cuSurfObjectDestroy(depth_surf);
    cuSurfObjectDestroy(shadow_surf);
    cuGraphicsUnmapResources(2, resources, 0);
    return true;
}

void RtLighting::composite(int screen_w, int screen_h, float shadow_strength) {
    if (!available_ || !shadow_gl_tex_) return;

    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, scene_fbo_);
    glBlitFramebuffer(0, 0, screen_w, screen_h,
                      0, 0, screen_w, screen_h,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

    glUseProgram(composite_prog_);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, scene_copy_tex_);
    glUniform1i(glGetUniformLocation(composite_prog_, "u_scene"), 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, shadow_gl_tex_);
    glUniform1i(glGetUniformLocation(composite_prog_, "u_shadow"), 1);

    glUniform1f(glGetUniformLocation(composite_prog_, "u_shadow_strength"), shadow_strength);

    glDisable(GL_DEPTH_TEST);
    glBindVertexArray(dummy_vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glEnable(GL_DEPTH_TEST);

    glUseProgram(0);
    glActiveTexture(GL_TEXTURE0);
}

// ---------------------------------------------------------------------------
// Phase 2: G-buffer FBO allocation and begin/end helpers
// ---------------------------------------------------------------------------
void RtLighting::resize_gbuffer(int w, int h) {
    if (w == gbuffer_w_ && h == gbuffer_h_) return;
    gbuffer_w_ = w;
    gbuffer_h_ = h;

    auto recreate_tex = [](unsigned& tex, GLenum ifmt, int w, int h, GLenum min_f, GLenum mag_f) {
        if (tex) glDeleteTextures(1, &tex);
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexStorage2D(GL_TEXTURE_2D, 1, ifmt, w, h);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min_f);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag_f);
    };
    recreate_tex(gbuffer_albedo_tex_, GL_RGBA8,               w, h, GL_NEAREST, GL_NEAREST);
    recreate_tex(gbuffer_normal_tex_, GL_RGBA16F,             w, h, GL_NEAREST, GL_NEAREST);
    recreate_tex(gbuffer_orm_tex_,    GL_RGBA8,               w, h, GL_NEAREST, GL_NEAREST);
    recreate_tex(gbuffer_depth_tex_,  GL_DEPTH_COMPONENT32F,  w, h, GL_NEAREST, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);

    if (gbuffer_fbo_) glDeleteFramebuffers(1, &gbuffer_fbo_);
    glGenFramebuffers(1, &gbuffer_fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, gbuffer_fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gbuffer_albedo_tex_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, gbuffer_normal_tex_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, gbuffer_orm_tex_,    0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,  GL_TEXTURE_2D, gbuffer_depth_tex_,  0);
    GLenum bufs[] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2 };
    glDrawBuffers(3, bufs);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
        printf("RtLighting: G-buffer FBO incomplete: 0x%x\n", status);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

bool RtLighting::begin_gbuffer(int screen_w, int screen_h) {
    if (!available_) return false;
    resize_gbuffer(screen_w, screen_h);
    glBindFramebuffer(GL_FRAMEBUFFER, gbuffer_fbo_);
    glViewport(0, 0, screen_w, screen_h);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    return true;
}

void RtLighting::end_gbuffer() {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// ---------------------------------------------------------------------------
// Phase 2 Task 3: build_lighting_pipeline
// ---------------------------------------------------------------------------
bool RtLighting::build_lighting_pipeline(std::string& err) {
    auto ctx = (OptixDeviceContext)optix_ctx_;

    OptixModuleCompileOptions mod_opts = {};
    mod_opts.maxRegisterCount = OPTIX_COMPILE_DEFAULT_MAX_REGISTER_COUNT;
    mod_opts.optLevel         = OPTIX_COMPILE_OPTIMIZATION_DEFAULT;

    OptixPipelineCompileOptions pipe_opts = {};
    pipe_opts.usesMotionBlur                   = false;
    pipe_opts.traversableGraphFlags            = OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_ANY;
    pipe_opts.numPayloadValues                 = 7;   // 6 for radiance + 1 for shadow
    pipe_opts.numAttributeValues               = 2;
    pipe_opts.exceptionFlags                   = OPTIX_EXCEPTION_FLAG_NONE;
    pipe_opts.pipelineLaunchParamsVariableName  = "params";

    char log[2048]; size_t log_size;
    auto create_module = [&](const char* ptx_str, OptixModule& mod) -> bool {
        log_size = sizeof(log);
        OptixResult r = optixModuleCreate(ctx, &mod_opts, &pipe_opts,
                                          ptx_str, strlen(ptx_str),
                                          log, &log_size, &mod);
        if (r != OPTIX_SUCCESS) {
            err = std::string("optixModuleCreate: ") + log;
            return false;
        }
        return true;
    };

    OptixModule mod_lr = nullptr, mod_lch = nullptr, mod_lm = nullptr;
    OptixModule mod_sh = nullptr, mod_sm = nullptr;
    if (!create_module(matter_rt_embedded::lighting_raygen_ptx,     mod_lr))  return false;
    if (!create_module(matter_rt_embedded::lighting_closesthit_ptx, mod_lch)) return false;
    if (!create_module(matter_rt_embedded::lighting_miss_ptx,       mod_lm))  return false;
    if (!create_module(matter_rt_embedded::shadow_hit_ptx,          mod_sh))  return false;
    if (!create_module(matter_rt_embedded::shadow_miss_ptx,         mod_sm))  return false;

    OptixProgramGroupOptions pg_opts = {};
    OptixProgramGroupDesc pg_desc = {};

    // Raygen: lighting
    pg_desc.kind = OPTIX_PROGRAM_GROUP_KIND_RAYGEN;
    pg_desc.raygen.module = mod_lr;
    pg_desc.raygen.entryFunctionName = "__raygen__lighting";
    log_size = sizeof(log);
    optixProgramGroupCreate(ctx, &pg_desc, 1, &pg_opts, log, &log_size,
                            (OptixProgramGroup*)&lighting_raygen_pg_);

    // Miss 0: shadow (no-op, payload stays 0)
    pg_desc = {};
    pg_desc.kind = OPTIX_PROGRAM_GROUP_KIND_MISS;
    pg_desc.miss.module = mod_sm;
    pg_desc.miss.entryFunctionName = "__miss__shadow";
    log_size = sizeof(log);
    OptixProgramGroup shadow_miss_pg = nullptr;
    optixProgramGroupCreate(ctx, &pg_desc, 1, &pg_opts, log, &log_size, &shadow_miss_pg);

    // Miss 1: radiance (returns sky color)
    pg_desc = {};
    pg_desc.kind = OPTIX_PROGRAM_GROUP_KIND_MISS;
    pg_desc.miss.module = mod_lm;
    pg_desc.miss.entryFunctionName = "__miss__radiance";
    log_size = sizeof(log);
    optixProgramGroupCreate(ctx, &pg_desc, 1, &pg_opts, log, &log_size,
                            (OptixProgramGroup*)&radiance_miss_pg_);

    // Hitgroup 1 (ray type 1): radiance closest-hit
    pg_desc = {};
    pg_desc.kind = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
    pg_desc.hitgroup.moduleCH            = mod_lch;
    pg_desc.hitgroup.entryFunctionNameCH = "__closesthit__radiance";
    log_size = sizeof(log);
    optixProgramGroupCreate(ctx, &pg_desc, 1, &pg_opts, log, &log_size,
                            (OptixProgramGroup*)&closesthit_pg_);

    OptixProgramGroup groups[] = {
        (OptixProgramGroup)lighting_raygen_pg_,
        shadow_miss_pg,
        (OptixProgramGroup)radiance_miss_pg_,
        (OptixProgramGroup)hit_pg_,       // shadow anyhit (from Phase 1)
        (OptixProgramGroup)closesthit_pg_
    };
    OptixPipelineLinkOptions link_opts = {};
    link_opts.maxTraceDepth = 2;  // raygen → GI bounce → nested shadow
    log_size = sizeof(log);
    optixPipelineCreate(ctx, &pipe_opts, &link_opts,
                        groups, 5, log, &log_size,
                        (OptixPipeline*)&lighting_pipeline_);

    // SBT: raygen record
    struct alignas(OPTIX_SBT_RECORD_ALIGNMENT) SbtRecord {
        char header[OPTIX_SBT_RECORD_HEADER_SIZE];
    };
    {
        SbtRecord rec = {};
        optixSbtRecordPackHeader((OptixProgramGroup)lighting_raygen_pg_, &rec);
        CUdeviceptr d = 0;
        cuMemAlloc(&d, sizeof(SbtRecord));
        cuMemcpyHtoD(d, &rec, sizeof(SbtRecord));
        sbt_lighting_raygen_ = (uint64_t)d;
    }

    // SBT: 2 miss records (shadow + radiance)
    {
        SbtRecord recs[2] = {};
        optixSbtRecordPackHeader(shadow_miss_pg, &recs[0]);
        optixSbtRecordPackHeader((OptixProgramGroup)radiance_miss_pg_, &recs[1]);
        CUdeviceptr d = 0;
        cuMemAlloc(&d, 2 * sizeof(SbtRecord));
        cuMemcpyHtoD(d, recs, 2 * sizeof(SbtRecord));
        sbt_lighting_miss_ = (uint64_t)d;
    }

    // Hitgroup SBT is built dynamically by rebuild_hitgroup_sbt() (Task 2).
    // Signal a rebuild now that closesthit_pg_ is real.
    sbt_dirty_ = true;

    return true;
}

// ---------------------------------------------------------------------------
// Phase 2 Task 3: trace_lighting
// ---------------------------------------------------------------------------
bool RtLighting::trace_lighting(const float inv_vp[16], const float sun_dir[3],
                                 const float sun_color[3], const float sky_color[3],
                                 int screen_w, int screen_h) {
    if (!available_ || !tlas_handle_ || trace_w_ == 0) return false;
    if (!lighting_pipeline_) return false;

    // Ensure lighting output texture exists at half-res.
    int new_tw = screen_w / 2;
    int new_th = screen_h / 2;
    if (new_tw < 1) new_tw = 1;
    if (new_th < 1) new_th = 1;

    if (!lighting_gl_tex_ || new_tw != trace_w_ || new_th != trace_h_) {
        resize(screen_w, screen_h);

        if (lighting_interop_registered_) {
            cuGraphicsUnregisterResource((CUgraphicsResource)cuda_lighting_resource_);
            cuGraphicsUnregisterResource((CUgraphicsResource)cuda_gb_albedo_resource_);
            cuGraphicsUnregisterResource((CUgraphicsResource)cuda_gb_normal_resource_);
            cuGraphicsUnregisterResource((CUgraphicsResource)cuda_gb_orm_resource_);
            lighting_interop_registered_ = false;
        }

        if (lighting_gl_tex_) glDeleteTextures(1, &lighting_gl_tex_);
        glGenTextures(1, &lighting_gl_tex_);
        glBindTexture(GL_TEXTURE_2D, lighting_gl_tex_);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA16F, trace_w_, trace_h_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindTexture(GL_TEXTURE_2D, 0);

        CUgraphicsResource res = nullptr;
        cuGraphicsGLRegisterImage(&res, lighting_gl_tex_,
                                  GL_TEXTURE_2D, CU_GRAPHICS_REGISTER_FLAGS_SURFACE_LDST);
        cuda_lighting_resource_ = (uint64_t)res;

        // Register G-buffer textures for CUDA read.
        CUgraphicsResource alb_res = nullptr, norm_res = nullptr, orm_res = nullptr;
        cuGraphicsGLRegisterImage(&alb_res, gbuffer_albedo_tex_,
                                  GL_TEXTURE_2D, CU_GRAPHICS_REGISTER_FLAGS_READ_ONLY);
        cuGraphicsGLRegisterImage(&norm_res, gbuffer_normal_tex_,
                                  GL_TEXTURE_2D, CU_GRAPHICS_REGISTER_FLAGS_READ_ONLY);
        cuGraphicsGLRegisterImage(&orm_res, gbuffer_orm_tex_,
                                  GL_TEXTURE_2D, CU_GRAPHICS_REGISTER_FLAGS_READ_ONLY);
        cuda_gb_albedo_resource_ = (uint64_t)alb_res;
        cuda_gb_normal_resource_ = (uint64_t)norm_res;
        cuda_gb_orm_resource_    = (uint64_t)orm_res;
        lighting_interop_registered_ = true;
    }

    // Map all CUDA resources.
    CUgraphicsResource resources[5] = {
        (CUgraphicsResource)cuda_depth_resource_,
        (CUgraphicsResource)cuda_lighting_resource_,
        (CUgraphicsResource)cuda_gb_albedo_resource_,
        (CUgraphicsResource)cuda_gb_normal_resource_,
        (CUgraphicsResource)cuda_gb_orm_resource_,
    };
    cuGraphicsMapResources(5, resources, 0);

    // Get CUDA arrays and create surface objects.
    auto get_surf = [](CUgraphicsResource res) -> CUsurfObject {
        CUarray arr = nullptr;
        cuGraphicsSubResourceGetMappedArray(&arr, res, 0, 0);
        CUDA_RESOURCE_DESC rd = {};
        rd.resType = CU_RESOURCE_TYPE_ARRAY;
        rd.res.array.hArray = arr;
        CUsurfObject s = 0;
        cuSurfObjectCreate(&s, &rd);
        return s;
    };

    CUsurfObject depth_surf    = get_surf((CUgraphicsResource)cuda_depth_resource_);
    CUsurfObject lighting_surf = get_surf((CUgraphicsResource)cuda_lighting_resource_);
    CUsurfObject albedo_surf   = get_surf((CUgraphicsResource)cuda_gb_albedo_resource_);
    CUsurfObject normal_surf   = get_surf((CUgraphicsResource)cuda_gb_normal_resource_);
    CUsurfObject orm_surf      = get_surf((CUgraphicsResource)cuda_gb_orm_resource_);

    // Fill launch params.
    RtLaunchParams lp = {};
    lp.tlas            = (OptixTraversableHandle)tlas_handle_;
    memcpy(lp.inv_vp, inv_vp, 16 * sizeof(float));
    memcpy(lp.sun_dir, sun_dir, 3 * sizeof(float));
    lp.width           = trace_w_;
    lp.height          = trace_h_;
    lp.depth_surface   = (unsigned long long)depth_surf;
    lp.shadow_surface  = 0;  // not used by lighting raygen
    lp.material_table  = (float*)(CUdeviceptr)d_material_table_;
    lp.material_count  = material_count_;
    lp.albedo_surface  = (unsigned long long)albedo_surf;
    lp.normal_surface  = (unsigned long long)normal_surf;
    lp.orm_surface     = (unsigned long long)orm_surf;
    lp.lighting_surface = (unsigned long long)lighting_surf;
    lp.screen_w        = screen_w;
    lp.screen_h        = screen_h;
    memcpy(lp.sun_color, sun_color, 3 * sizeof(float));
    memcpy(lp.sky_color, sky_color, 3 * sizeof(float));
    lp.frame_index     = frame_index_;

    cuMemcpyHtoD((CUdeviceptr)d_params_, &lp, sizeof(RtLaunchParams));

    // Ensure hitgroup SBT is current (closesthit_pg_ is now real after build_lighting_pipeline).
    rebuild_hitgroup_sbt();

    // Build SBT for the lighting pipeline.
    struct alignas(OPTIX_SBT_RECORD_ALIGNMENT) SbtRecord {
        char header[OPTIX_SBT_RECORD_HEADER_SIZE];
    };
    struct alignas(OPTIX_SBT_RECORD_ALIGNMENT) HitGroupRecord {
        char header[OPTIX_SBT_RECORD_HEADER_SIZE];
        HitGroupData data;
    };

    OptixShaderBindingTable sbt = {};
    sbt.raygenRecord                = (CUdeviceptr)sbt_lighting_raygen_;
    sbt.missRecordBase              = (CUdeviceptr)sbt_lighting_miss_;
    sbt.missRecordStrideInBytes     = sizeof(SbtRecord);
    sbt.missRecordCount             = 2;  // shadow miss + radiance miss
    sbt.hitgroupRecordBase          = (CUdeviceptr)d_hitgroup_sbt_;
    sbt.hitgroupRecordStrideInBytes = sizeof(HitGroupRecord);
    sbt.hitgroupRecordCount         = (unsigned int)(next_blas_sbt_index_ * 2);

    optixLaunch((OptixPipeline)lighting_pipeline_, 0,
                (CUdeviceptr)d_params_, sizeof(RtLaunchParams),
                &sbt, trace_w_, trace_h_, 1);
    cuCtxSynchronize();

    // Cleanup surface objects.
    cuSurfObjectDestroy(depth_surf);
    cuSurfObjectDestroy(lighting_surf);
    cuSurfObjectDestroy(albedo_surf);
    cuSurfObjectDestroy(normal_surf);
    cuSurfObjectDestroy(orm_surf);
    cuGraphicsUnmapResources(5, resources, 0);
    return true;
}

// ---------------------------------------------------------------------------
// Phase 2 Task 4: composite_lighting — tonemapped fullscreen blit to default FB
// ---------------------------------------------------------------------------
void RtLighting::composite_lighting(int screen_w, int screen_h) {
    if (!available_ || !lighting_gl_tex_ || !lighting_composite_prog_) return;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, screen_w, screen_h);

    glUseProgram(lighting_composite_prog_);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gbuffer_albedo_tex_);
    glUniform1i(glGetUniformLocation(lighting_composite_prog_, "u_albedo"), 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, lighting_gl_tex_);
    glUniform1i(glGetUniformLocation(lighting_composite_prog_, "u_lighting"), 1);

    glDisable(GL_DEPTH_TEST);
    glBindVertexArray(dummy_vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glEnable(GL_DEPTH_TEST);

    glUseProgram(0);
    glActiveTexture(GL_TEXTURE0);
}

// ---------------------------------------------------------------------------
// Phase 2 Task 5: init_denoiser
// ---------------------------------------------------------------------------
bool RtLighting::init_denoiser(std::string& err) {
    auto ctx = (OptixDeviceContext)optix_ctx_;

    OptixDenoiserOptions opts = {};
    opts.guideAlbedo = 1;
    opts.guideNormal = 1;

    OptixResult r = optixDenoiserCreate(ctx,
        OPTIX_DENOISER_MODEL_KIND_HDR, &opts,
        (OptixDenoiser*)&denoiser_);
    if (r != OPTIX_SUCCESS) {
        err = "optixDenoiserCreate failed: " + std::to_string((int)r);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Phase 2 Task 5: accumulate — increment frame index, store prev VP
// ---------------------------------------------------------------------------
void RtLighting::accumulate(const float inv_vp[16]) {
    int n = trace_w_ * trace_h_;
    size_t buf_bytes = (size_t)n * 4 * sizeof(float);

    // Allocate persistent buffers if needed.
    if (accum_w_ != trace_w_ || accum_h_ != trace_h_) {
        if (d_accum_buffer_)   cuMemFree((CUdeviceptr)d_accum_buffer_);
        if (d_current_buffer_) cuMemFree((CUdeviceptr)d_current_buffer_);
        CUdeviceptr a = 0, c = 0;
        cuMemAlloc(&a, buf_bytes);
        cuMemAlloc(&c, buf_bytes);
        cuMemsetD8((CUdeviceptr)a, 0, buf_bytes);
        d_accum_buffer_   = (uint64_t)a;
        d_current_buffer_ = (uint64_t)c;
        accum_w_ = trace_w_;
        accum_h_ = trace_h_;
        frame_index_ = 0;
        have_prev_vp_ = false;
    }

    // Simple approach: just increment frame_index for temporal variation.
    // The denoiser handles the single-sample noise.
    frame_index_++;
    memcpy(prev_vp_, inv_vp, 16 * sizeof(float));
    have_prev_vp_ = true;
}

// ---------------------------------------------------------------------------
// Phase 2 Task 5: denoise — no-op pass-through
// Full denoiser integration requires an RGBA16F→RGBA32F conversion kernel.
// ---------------------------------------------------------------------------
void RtLighting::denoise() {
    if (!denoiser_ || trace_w_ == 0) return;
    auto ctx = (OptixDeviceContext)optix_ctx_;

    size_t pixel_bytes = trace_w_ * trace_h_ * 4 * sizeof(float);

    // Allocate denoiser buffers on first use or resize.
    OptixDenoiserSizes sizes = {};
    optixDenoiserComputeMemoryResources(
        (OptixDenoiser)denoiser_, trace_w_, trace_h_, &sizes);

    if (sizes.stateSizeInBytes > denoiser_state_size_) {
        if (d_denoiser_state_) cuMemFree((CUdeviceptr)d_denoiser_state_);
        CUdeviceptr d = 0;
        cuMemAlloc(&d, sizes.stateSizeInBytes);
        d_denoiser_state_ = (uint64_t)d;
        denoiser_state_size_ = sizes.stateSizeInBytes;

        optixDenoiserSetup((OptixDenoiser)denoiser_, 0,
                           trace_w_, trace_h_,
                           (CUdeviceptr)d_denoiser_state_, sizes.stateSizeInBytes,
                           (CUdeviceptr)d_denoiser_scratch_, sizes.withoutOverlapScratchSizeInBytes);
    }
    if (sizes.withoutOverlapScratchSizeInBytes > denoiser_scratch_size_) {
        if (d_denoiser_scratch_) cuMemFree((CUdeviceptr)d_denoiser_scratch_);
        CUdeviceptr d = 0;
        cuMemAlloc(&d, sizes.withoutOverlapScratchSizeInBytes);
        d_denoiser_scratch_ = (uint64_t)d;
        denoiser_scratch_size_ = sizes.withoutOverlapScratchSizeInBytes;
    }
    if (!d_denoised_buffer_) {
        CUdeviceptr d = 0;
        cuMemAlloc(&d, pixel_bytes);
        d_denoised_buffer_ = (uint64_t)d;
    }
    if (!d_denoise_albedo_) {
        CUdeviceptr d = 0;
        cuMemAlloc(&d, pixel_bytes);
        d_denoise_albedo_ = (uint64_t)d;
    }
    if (!d_denoise_normal_) {
        CUdeviceptr d = 0;
        cuMemAlloc(&d, pixel_bytes);
        d_denoise_normal_ = (uint64_t)d;
    }

    // Read the noisy lighting from the GL texture into a CUDA buffer.
    // Map the lighting resource, copy array → linear buffer.
    CUgraphicsResource res = (CUgraphicsResource)cuda_lighting_resource_;
    cuGraphicsMapResources(1, &res, 0);
    CUarray arr = nullptr;
    cuGraphicsSubResourceGetMappedArray(&arr, res, 0, 0);

    CUDA_MEMCPY2D cpy = {};
    cpy.srcMemoryType = CU_MEMORYTYPE_ARRAY;
    cpy.srcArray      = arr;
    cpy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
    cpy.dstDevice     = (CUdeviceptr)d_current_buffer_;
    cpy.dstPitch      = trace_w_ * 4 * sizeof(float);
    cpy.WidthInBytes  = trace_w_ * 4 * sizeof(float);   // RGBA32F
    cpy.Height        = trace_h_;

    // Note: lighting texture is RGBA16F, but denoiser wants RGBA32F.
    // We'd need a conversion kernel. For the initial implementation,
    // skip the denoiser and just use the raw lighting output.
    // TODO: Add a CUDA kernel to convert RGBA16F → RGBA32F for denoiser input.

    cuGraphicsUnmapResources(1, &res, 0);

    // For now, the denoiser is a no-op pass-through.
    // The raw 1-spp lighting goes directly to the composite.
    // Full denoiser integration requires an RGBA16F→RGBA32F conversion kernel
    // and reading G-buffer albedo/normal into guide buffers.
    (void)ctx;
    (void)cpy;
}

// ---------------------------------------------------------------------------
// Phase 2 Task 5: copy_denoised_to_gl — no-op pass-through
// ---------------------------------------------------------------------------
void RtLighting::copy_denoised_to_gl() {
    // When the denoiser is fully implemented, this copies the denoised
    // RGBA32F buffer back to the lighting GL texture (RGBA16F).
    // For now, the raw lighting output is already in the GL texture
    // from trace_lighting(), so this is a no-op.
}

// ---------------------------------------------------------------------------
// Phase 2 Task 5: accumulate_and_denoise — public entry point
// ---------------------------------------------------------------------------
void RtLighting::accumulate_and_denoise(const float inv_vp[16]) {
    accumulate(inv_vp);
    denoise();
    copy_denoised_to_gl();
}

RtLighting::~RtLighting() { shutdown(); }

} // namespace viewer

#endif // MATTER_HAVE_OPTIX
