#include <cuda_runtime.h>

extern "C" __global__ void matter_classify_fluid_particles(
    const float4* positions,
    unsigned int particle_count,
    float sensor_min_x, float sensor_min_y, float sensor_min_z,
    float sensor_max_x, float sensor_max_y, float sensor_max_z,
    unsigned int sensor_cells_x, unsigned int sensor_cells_z,
    float collar_min_x, float collar_min_y, float collar_min_z,
    float collar_max_x, float collar_max_y, float collar_max_z,
    unsigned int* column_counts,
    unsigned int* escaped_count,
    unsigned int* non_finite_count) {
    const unsigned int index = blockIdx.x * blockDim.x + threadIdx.x;
    if (index >= particle_count) return;

    const float4 value = positions[index];
    if (!isfinite(value.x) || !isfinite(value.y) || !isfinite(value.z)) {
        atomicAdd(non_finite_count, 1u);
        return;
    }
    if (value.x < collar_min_x || value.x > collar_max_x ||
        value.y < collar_min_y || value.y > collar_max_y ||
        value.z < collar_min_z || value.z > collar_max_z) {
        atomicAdd(escaped_count, 1u);
    }
    if (value.x < sensor_min_x || value.x >= sensor_max_x ||
        value.y < sensor_min_y || value.y >= sensor_max_y ||
        value.z < sensor_min_z || value.z >= sensor_max_z) {
        return;
    }

    const float normalized_x =
        (value.x - sensor_min_x) / (sensor_max_x - sensor_min_x);
    const float normalized_z =
        (value.z - sensor_min_z) / (sensor_max_z - sensor_min_z);
    const unsigned int cell_x = min(
        static_cast<unsigned int>(normalized_x * sensor_cells_x),
        sensor_cells_x - 1u);
    const unsigned int cell_z = min(
        static_cast<unsigned int>(normalized_z * sensor_cells_z),
        sensor_cells_z - 1u);
    atomicAdd(&column_counts[cell_z * sensor_cells_x + cell_x], 1u);
}

extern "C" __global__ void matter_count_wet_fluid_columns(
    const unsigned int* column_counts,
    unsigned int column_count,
    unsigned int minimum_particles_per_cell,
    unsigned int* wet_count) {
    const unsigned int index = blockIdx.x * blockDim.x + threadIdx.x;
    if (index < column_count &&
        column_counts[index] >= minimum_particles_per_cell) {
        atomicAdd(wet_count, 1u);
    }
}
