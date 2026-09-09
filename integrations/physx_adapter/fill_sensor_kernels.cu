#include <cuda_runtime.h>

extern "C" __global__ void matter_classify_fluid_particles(
    const float4* positions,
    unsigned int particle_count,
    float sensor_origin_x, float sensor_origin_y, float sensor_origin_z,
    float sensor_longitudinal_x, float sensor_longitudinal_z,
    float sensor_lateral_x, float sensor_lateral_z,
    float sensor_extent_x, float sensor_extent_y, float sensor_extent_z,
    unsigned int sensor_cells_x, unsigned int sensor_cells_z,
    float collar_min_x, float collar_min_y, float collar_min_z,
    float collar_max_x, float collar_max_y, float collar_max_z,
    unsigned int* quarantined,
    float4* quarantine_positions,
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
        if (atomicCAS(&quarantined[index], 0u, 1u) == 0u) {
            quarantine_positions[index] = value;
            atomicAdd(escaped_count, 1u);
        }
        return;
    }
    if (quarantined[index] != 0u) return;
    const float delta_x = value.x - sensor_origin_x;
    const float delta_z = value.z - sensor_origin_z;
    const float local_x = delta_x * sensor_longitudinal_x +
                          delta_z * sensor_longitudinal_z;
    const float local_y = value.y - sensor_origin_y;
    const float local_z = delta_x * sensor_lateral_x +
                          delta_z * sensor_lateral_z;
    if (local_x < 0.0f || local_x >= sensor_extent_x ||
        local_y < 0.0f || local_y >= sensor_extent_y ||
        local_z < 0.0f || local_z >= sensor_extent_z) {
        return;
    }

    const float normalized_x = local_x / sensor_extent_x;
    const float normalized_z = local_z / sensor_extent_z;
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
