#include "rt_lighting.h"

#ifdef MATTER_HAVE_OPTIX

#include <cuda.h>
#include <cudaGL.h>
#include <optix.h>
#include <optix_stubs.h>
#include <optix_function_table_definition.h>

#include <cstdio>

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
    return true;
}

void RtLighting::shutdown() {
    // Free all BLAS resources before destroying the OptiX context.
    for (auto& [_, blas] : blas_cache_) {
        cuMemFree((CUdeviceptr)blas.d_vertices);
        cuMemFree((CUdeviceptr)blas.gas_buffer);
    }
    blas_cache_.clear();

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
    unsigned int flags[1]   = { OPTIX_GEOMETRY_FLAG_DISABLE_ANYHIT };
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

RtLighting::~RtLighting() { shutdown(); }

} // namespace viewer

#endif // MATTER_HAVE_OPTIX
