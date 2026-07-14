#include <cuda_fp16.h>
#include <cuda_runtime.h>

extern "C" __global__ void vk_cuda_invert(cudaSurfaceObject_t surface,
                                            unsigned width,
                                            unsigned height) {
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;
    ushort4 value;
    surf2Dread(&value, surface, x * sizeof(value), y);
    value.x = __half_as_ushort(__float2half(1.0f - __half2float(__ushort_as_half(value.x))));
    value.y = __half_as_ushort(__float2half(1.0f - __half2float(__ushort_as_half(value.y))));
    value.z = __half_as_ushort(__float2half(1.0f - __half2float(__ushort_as_half(value.z))));
    surf2Dwrite(value, surface, x * sizeof(value), y);
}
