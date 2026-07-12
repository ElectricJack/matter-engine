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
    if (depth_lin_prog_)   { glDeleteProgram(depth_lin_prog_);        depth_lin_prog_ = 0; }
    if (composite_prog_)   { glDeleteProgram(composite_prog_);        composite_prog_ = 0; }

    // Phase 2: G-buffer cleanup.
    if (gbuffer_fbo_)        { glDeleteFramebuffers(1, &gbuffer_fbo_);          gbuffer_fbo_ = 0; }
    if (gbuffer_albedo_tex_) { glDeleteTextures(1, &gbuffer_albedo_tex_);        gbuffer_albedo_tex_ = 0; }
    if (gbuffer_normal_tex_) { glDeleteTextures(1, &gbuffer_normal_tex_);        gbuffer_normal_tex_ = 0; }
    if (gbuffer_orm_tex_)    { glDeleteTextures(1, &gbuffer_orm_tex_);           gbuffer_orm_tex_ = 0; }
    if (gbuffer_depth_tex_)  { glDeleteTextures(1, &gbuffer_depth_tex_);         gbuffer_depth_tex_ = 0; }
    gbuffer_w_ = gbuffer_h_ = 0;

    // Free all BLAS resources before destroying the OptiX context.
    for (auto& [_, blas] : blas_cache_) {
        cuMemFree((CUdeviceptr)blas.d_vertices);
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

void RtLighting::register_part(uint64_t part_hash, const float* vertices, int vertex_count) {
    if (!available_ || blas_cache_.count(part_hash)) return;

    RtBlas blas;
    blas.vertex_count = vertex_count;

    size_t verts_bytes = (size_t)vertex_count * 3 * sizeof(float);
    CUdeviceptr d_verts = 0;
    cuMemAlloc(&d_verts, verts_bytes);
    cuMemcpyHtoD(d_verts, vertices, verts_bytes);
    blas.d_vertices = (uint64_t)d_verts;

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

    blas.gas_buffer  = (uint64_t)d_compacted;
    blas.traversable = (uint64_t)handle;
    blas_cache_[part_hash] = blas;
}

void RtLighting::unregister_part(uint64_t part_hash) {
    auto it = blas_cache_.find(part_hash);
    if (it == blas_cache_.end()) return;
    cuMemFree((CUdeviceptr)it->second.d_vertices);
    cuMemFree((CUdeviceptr)it->second.gas_buffer);
    blas_cache_.erase(it);
}

void RtLighting::update_instances(const InstanceInput* instances, int count) {
    if (!available_ || count == 0) return;

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
        inst.sbtOffset         = 0;
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

    // Composite vertex + fragment shader.
    {
        std::string vs_src, vs_err, fs_src, fs_err;
        if (!matter::shader_text("shaders_gpu/rt_composite.vs", vs_src, vs_err)) {
            err = "rt_composite.vs not found: " + vs_err;
            return false;
        }
        if (!matter::shader_text("shaders_gpu/rt_composite.fs", fs_src, fs_err)) {
            err = "rt_composite.fs not found: " + fs_err;
            return false;
        }
        GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src.c_str());
        GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src.c_str());
        if (!vs || !fs) { err = "rt_composite compile failed"; return false; }
        composite_prog_ = link_program(vs, fs);
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

    OptixShaderBindingTable sbt = {};
    sbt.raygenRecord                = (CUdeviceptr)sbt_raygen_buf_;
    sbt.missRecordBase              = (CUdeviceptr)sbt_miss_buf_;
    sbt.missRecordStrideInBytes     = OPTIX_SBT_RECORD_HEADER_SIZE;
    sbt.missRecordCount             = 1;
    sbt.hitgroupRecordBase          = (CUdeviceptr)sbt_hit_buf_;
    sbt.hitgroupRecordStrideInBytes = OPTIX_SBT_RECORD_HEADER_SIZE;
    sbt.hitgroupRecordCount         = 1;

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

RtLighting::~RtLighting() { shutdown(); }

} // namespace viewer

#endif // MATTER_HAVE_OPTIX
