#include "gpu_visual_mesher_vk.h"

#include "render/vk_pipeline.h"
#include "render/vk_resources.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <utility>

namespace gpu_meshing {

// Use the same immutable table as the CPU mesher; it no longer exports a
// mutable C symbol from surface.c.
#include "mc_tables.h"

namespace {

constexpr std::uint32_t kScanWorkgroup = 256u;

double elapsed_ms(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - start)
        .count();
}

std::size_t align_up_256(std::size_t value) {
    constexpr std::size_t kAlignment = 256u;
    if (value > std::numeric_limits<std::size_t>::max() -
                    (kAlignment - 1u))
        return 0u;
    return (value + kAlignment - 1u) & ~(kAlignment - 1u);
}

bool fail(Error& error, ErrorCode code, std::string message) {
    error.code = code;
    error.message = std::move(message);
    return false;
}

ErrorCode classify_vulkan_error(const std::string& message) {
    return message.find("VkResult -4") != std::string::npos
               ? ErrorCode::DeviceLost
               : ErrorCode::VulkanFailure;
}

bool check_control(const BuildControl& control, std::uint64_t generation,
                   Error& error) {
    if (control.cancelled && control.cancelled())
        return fail(error, ErrorCode::Cancelled,
                    "GPU visual mesh build was cancelled");
    if (control.generation_is_current &&
        !control.generation_is_current(generation)) {
        return fail(error, ErrorCode::StaleGeneration,
                    "GPU visual mesh build generation is stale");
    }
    return true;
}

#ifdef MATTER_VK_TEST_FAULT_INJECTION
bool forced_fault(const char* phase) {
    const char* selected = std::getenv("MATTER_GPU_MESH_TEST_FAULT");
    return selected != nullptr && std::string(selected) == phase;
}
#endif

VkDescriptorSetLayoutBinding storage_binding(std::uint32_t binding) {
    VkDescriptorSetLayoutBinding result{};
    result.binding = binding;
    result.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    result.descriptorCount = 1;
    result.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    return result;
}

bool ensure_pipeline(matter::VulkanDevice& vulkan,
                     matter::VkComputePipelineResource& pipeline,
                     const char* shader, std::uint32_t binding_count,
                     Error& error) {
    if (pipeline.pipeline != VK_NULL_HANDLE) return true;
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    bindings.reserve(binding_count);
    for (std::uint32_t binding = 0; binding != binding_count; ++binding)
        bindings.push_back(storage_binding(binding));
    std::string vk_error;
    if (!matter::create_compute_pipeline(vulkan, shader, bindings, pipeline,
                                         vk_error)) {
        return fail(error, classify_vulkan_error(vk_error),
                    "GPU mesher pipeline creation failed: " + vk_error);
    }
    return true;
}

bool create_gpu_buffer(matter::VulkanDevice& vulkan, std::size_t bytes,
                       matter::VkBufferResource& output, Error& error) {
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    if (forced_fault("allocation"))
        return fail(error, ErrorCode::VulkanFailure,
                    "forced GPU mesher allocation failure");
#endif
    if (bytes == 0 || bytes >
                          static_cast<std::size_t>(
                              std::numeric_limits<VkDeviceSize>::max())) {
        return fail(error, ErrorCode::Overflow,
                    "GPU mesher buffer size is invalid");
    }
    std::string vk_error;
    if (!matter::create_buffer(
            vulkan, static_cast<VkDeviceSize>(bytes),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, output, vk_error)) {
        return fail(error, classify_vulkan_error(vk_error),
                    "GPU mesher buffer allocation failed: " + vk_error);
    }
    return true;
}

bool upload(matter::VulkanDevice& vulkan, matter::VkBufferResource& buffer,
            const void* bytes, std::size_t byte_count, Error& error) {
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    if (forced_fault("upload"))
        return fail(error, ErrorCode::VulkanFailure,
                    "forced GPU mesher upload failure");
#endif
    std::string vk_error;
    if (!matter::upload_buffer(vulkan, buffer, bytes, byte_count, 0,
                               vk_error)) {
        return fail(error, classify_vulkan_error(vk_error),
                    "GPU mesher upload failed: " + vk_error);
    }
    return true;
}

bool readback(matter::VulkanDevice& vulkan, matter::VkBufferResource& buffer,
              void* bytes, std::size_t byte_count, Error& error) {
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    if (forced_fault("readback"))
        return fail(error, ErrorCode::VulkanFailure,
                    "forced GPU mesher readback failure");
#endif
    std::string vk_error;
    if (!matter::readback_buffer(vulkan, buffer, bytes, byte_count, 0,
                                 vk_error)) {
        return fail(error, classify_vulkan_error(vk_error),
                    "GPU mesher readback failed: " + vk_error);
    }
    return true;
}

struct alignas(16) ScanParams {
    std::uint32_t count = 0;
    std::uint32_t block_count = 0;
    std::uint32_t unused0 = 0;
    std::uint32_t unused1 = 0;
};

struct alignas(16) BinParams {
    std::array<float, 4> bin_origin_and_size{};
    std::array<std::uint32_t, 4> bin_dims_and_count{};
    std::array<std::uint32_t, 4> counts{};
};

struct alignas(16) FieldParams {
    std::array<float, 4> origin_and_iso{};
    std::array<float, 4> spacing_and_blend{};
    std::array<float, 4> bin_origin_and_size{};
    std::array<std::uint32_t, 4> sample_dims_and_count{};
    std::array<std::uint32_t, 4> bin_dims_and_count{};
    std::array<std::uint32_t, 4> counts{};
    std::array<float, 4> query_radius_and_padding{};
    std::array<std::uint32_t, 4> source0_phase_spans{};
    std::array<std::uint32_t, 4> source1_phase_spans{};
    std::array<float, 4> source_blend_origin_and_enabled{};
    std::array<float, 4> source_blend_direction{};
    std::array<float, 4> source_blend_distances_and_padding{};
};

static_assert(sizeof(ScanParams) == 16, "scan params must match one uvec4");
static_assert(sizeof(BinParams) == 48,
              "bin params must match three std430 vec4 values");
static_assert(sizeof(FieldParams) == 192,
              "field params must match twelve std430 vec4 values");
// FieldParams::counts.z and query_radius_and_padding.yz are the phase split
// and weights declared by GpuMeshFieldParams in gpu_mesh_common.glsl.
static_assert(sizeof(ParticleSample) == 16,
              "particle samples must match the GLSL particle ABI");

}  // namespace

struct GpuVisualMesher::Impl {
    explicit Impl(matter::VulkanDevice& owner) : vulkan(owner) {}

    matter::VulkanDevice& vulkan;
    matter::VkComputePipelineResource scan_blocks;
    matter::VkComputePipelineResource scan_add;
    matter::VkComputePipelineResource bin_count;
    matter::VkComputePipelineResource bin_scatter;
    matter::VkComputePipelineResource bin_sort;
    matter::VkComputePipelineResource field;
    matter::VkComputePipelineResource classify;
    matter::VkComputePipelineResource compact;
    matter::VkComputePipelineResource emit;

    struct ResidentBins {
        GridLayout layout{};
        std::uint32_t contributing_particles = 0;
        matter::VkBufferResource particle_buffer;
        matter::VkBufferResource counts_buffer;
        matter::VkBufferResource offsets_buffer;
        matter::VkBufferResource ids_buffer;
    };

    struct ResidentField {
        ResidentBins bins;
        FieldParams params{};
        matter::VkBufferResource params_buffer;
        matter::VkBufferResource field_buffer;
        double bin_ms = 0.0;
        double field_ms = 0.0;
    };

    bool dispatch(matter::VkComputePipelineResource& pipeline,
                  std::uint32_t x, const BuildControl& control,
                  std::uint64_t generation, Error& error) {
        if (!check_control(control, generation, error)) return false;
#ifdef MATTER_VK_TEST_FAULT_INJECTION
        if (forced_fault("device-lost"))
            return fail(error, ErrorCode::DeviceLost,
                        "forced GPU mesher device-lost failure");
#endif
        std::string vk_error;
        if (!matter::dispatch_compute(vulkan, pipeline, x, 1u, 1u,
                                      vk_error)) {
            return fail(error, classify_vulkan_error(vk_error),
                        "GPU mesher dispatch failed: " + vk_error);
        }
        return check_control(control, generation, error);
    }

    bool scan_resource(matter::VkBufferResource& input, std::uint32_t count,
                       matter::VkBufferResource& output, std::uint32_t& total,
                       Error& error, const BuildControl& control,
                       std::uint64_t generation) {
        total = 0;
        if (count == 0) return true;
        const std::uint32_t blocks =
            (count + kScanWorkgroup - 1u) / kScanWorkgroup;
        if (!create_gpu_buffer(vulkan, sizeof(std::uint32_t) * count, output,
                               error))
            return false;
        matter::VkBufferResource block_sums;
        matter::VkBufferResource params_buffer;
        if (!create_gpu_buffer(vulkan, sizeof(std::uint32_t) * blocks,
                               block_sums, error) ||
            !create_gpu_buffer(vulkan, sizeof(ScanParams), params_buffer,
                               error))
            return false;
        const ScanParams params{count, blocks, 0u, 0u};
        if (!upload(vulkan, params_buffer, &params, sizeof(params), error) ||
            !ensure_pipeline(vulkan, scan_blocks,
                             "gpu_mesh_scan_blocks.comp.spv", 4u, error))
            return false;
        matter::write_storage_buffer_descriptor(scan_blocks, 0u,
                                                params_buffer, 0u,
                                                params_buffer.size);
        matter::write_storage_buffer_descriptor(scan_blocks, 1u, input, 0u,
                                                input.size);
        matter::write_storage_buffer_descriptor(scan_blocks, 2u, output, 0u,
                                                output.size);
        matter::write_storage_buffer_descriptor(scan_blocks, 3u, block_sums,
                                                0u, block_sums.size);
        if (!dispatch(scan_blocks, blocks, control, generation, error))
            return false;

        if (blocks == 1u) {
            return readback(vulkan, block_sums, &total, sizeof(total), error);
        }

        matter::VkBufferResource block_offsets;
        if (!scan_resource(block_sums, blocks, block_offsets, total, error,
                           control, generation))
            return false;
        if (!ensure_pipeline(vulkan, scan_add, "gpu_mesh_scan_add.comp.spv",
                             3u, error))
            return false;
        matter::write_storage_buffer_descriptor(scan_add, 0u, params_buffer,
                                                0u, params_buffer.size);
        matter::write_storage_buffer_descriptor(scan_add, 1u, output, 0u,
                                                output.size);
        matter::write_storage_buffer_descriptor(scan_add, 2u, block_offsets,
                                                0u, block_offsets.size);
        return dispatch(scan_add, blocks, control, generation, error);
    }

    bool scan_vector(const std::vector<std::uint32_t>& input,
                     std::vector<std::uint32_t>& output,
                     std::uint32_t& total, Error& error) {
        output.clear();
        total = 0;
        error = {};
        if (input.empty()) return true;
        std::vector<std::uint32_t> ignored;
        std::uint32_t reference_total = 0;
        if (!exclusive_scan_reference(input, ignored, reference_total))
            return fail(error, ErrorCode::Overflow,
                        "GPU scan input sum overflows uint32");
        matter::VkBufferResource input_buffer;
        matter::VkBufferResource output_buffer;
        if (!create_gpu_buffer(vulkan, input.size() * sizeof(std::uint32_t),
                               input_buffer, error) ||
            !upload(vulkan, input_buffer, input.data(),
                    input.size() * sizeof(std::uint32_t), error) ||
            !scan_resource(input_buffer, static_cast<std::uint32_t>(input.size()),
                           output_buffer, total, error, {}, 0u))
            return false;
        std::vector<std::uint32_t> candidate(input.size());
        if (!readback(vulkan, output_buffer, candidate.data(),
                      candidate.size() * sizeof(std::uint32_t), error))
            return false;
        if (total != reference_total)
            return fail(error, ErrorCode::VulkanFailure,
                        "GPU scan total disagrees with checked input sum");
        output = std::move(candidate);
        return true;
    }

    bool build_bins(const ParticleJob& job, GpuParticleBins& bins,
                    Error& error, const BuildControl& control = {},
                    ResidentBins* resident = nullptr) {
        bins = {};
        error = {};
        GridLayout layout{};
        if (!validate_particle_job(job, layout, error)) return false;
        bins.layout = layout;
        if (job.particle_count == 0) return true;

        matter::VkBufferResource params_buffer;
        matter::VkBufferResource particle_buffer;
        matter::VkBufferResource counts_buffer;
        matter::VkBufferResource offsets_buffer;
        matter::VkBufferResource cursors_buffer;
        matter::VkBufferResource ids_buffer;
        const std::size_t bins_bytes =
            static_cast<std::size_t>(layout.bins) * sizeof(std::uint32_t);
        const std::size_t particles_bytes =
            static_cast<std::size_t>(job.particle_count) * sizeof(ParticleSample);
        if (!create_gpu_buffer(vulkan, sizeof(BinParams), params_buffer,
                               error) ||
            !create_gpu_buffer(vulkan, particles_bytes, particle_buffer,
                               error) ||
            !create_gpu_buffer(vulkan, bins_bytes, counts_buffer, error) ||
            !create_gpu_buffer(vulkan, bins_bytes, cursors_buffer, error))
            return false;

        const BinParams params{
            {layout.bin_origin_m.x, layout.bin_origin_m.y,
             layout.bin_origin_m.z, layout.bin_size_m},
            {layout.bin_dims[0], layout.bin_dims[1], layout.bin_dims[2],
             layout.bins},
            {job.particle_count, resolved_particle_phase_split(job),
             job.phase_blend.primary_weight != 0.0f ? 1u : 0u,
             job.phase_blend.secondary_weight != 0.0f ? 1u : 0u},
        };
        std::vector<std::uint32_t> zeros(layout.bins, 0u);
        if (!upload(vulkan, params_buffer, &params, sizeof(params), error) ||
            !upload(vulkan, particle_buffer, job.particles, particles_bytes,
                    error) ||
            !upload(vulkan, counts_buffer, zeros.data(), bins_bytes, error) ||
            !upload(vulkan, cursors_buffer, zeros.data(), bins_bytes, error) ||
            !ensure_pipeline(vulkan, bin_count,
                             "gpu_mesh_bin_count.comp.spv", 3u, error))
            return false;

        matter::write_storage_buffer_descriptor(bin_count, 0u, params_buffer,
                                                0u, params_buffer.size);
        matter::write_storage_buffer_descriptor(bin_count, 1u, particle_buffer,
                                                0u, particle_buffer.size);
        matter::write_storage_buffer_descriptor(bin_count, 2u, counts_buffer,
                                                0u, counts_buffer.size);
        const std::uint32_t particle_groups =
            (job.particle_count + 255u) / 256u;
        if (!dispatch(bin_count, particle_groups, control, job.generation,
                      error))
            return false;

        std::uint32_t contributing_particles = 0;
        if (!scan_resource(counts_buffer, layout.bins, offsets_buffer,
                           contributing_particles, error, control,
                           job.generation))
            return false;
        const std::size_t id_capacity =
            std::max<std::size_t>(contributing_particles, 1u);
        if (!create_gpu_buffer(vulkan, id_capacity * sizeof(std::uint32_t),
                               ids_buffer, error) ||
            !ensure_pipeline(vulkan, bin_scatter,
                             "gpu_mesh_bin_scatter.comp.spv", 6u, error))
            return false;
        matter::write_storage_buffer_descriptor(bin_scatter, 0u, params_buffer,
                                                0u, params_buffer.size);
        matter::write_storage_buffer_descriptor(bin_scatter, 1u,
                                                particle_buffer, 0u,
                                                particle_buffer.size);
        matter::write_storage_buffer_descriptor(bin_scatter, 2u, counts_buffer,
                                                0u, counts_buffer.size);
        matter::write_storage_buffer_descriptor(bin_scatter, 3u, offsets_buffer,
                                                0u, offsets_buffer.size);
        matter::write_storage_buffer_descriptor(bin_scatter, 4u, cursors_buffer,
                                                0u, cursors_buffer.size);
        matter::write_storage_buffer_descriptor(bin_scatter, 5u, ids_buffer,
                                                0u, ids_buffer.size);
        if (!dispatch(bin_scatter, particle_groups, control, job.generation,
                      error) ||
            !ensure_pipeline(vulkan, bin_sort, "gpu_mesh_bin_sort.comp.spv",
                             4u, error))
            return false;
        matter::write_storage_buffer_descriptor(bin_sort, 0u, params_buffer,
                                                0u, params_buffer.size);
        matter::write_storage_buffer_descriptor(bin_sort, 1u, counts_buffer,
                                                0u, counts_buffer.size);
        matter::write_storage_buffer_descriptor(bin_sort, 2u, offsets_buffer,
                                                0u, offsets_buffer.size);
        matter::write_storage_buffer_descriptor(bin_sort, 3u, ids_buffer, 0u,
                                                ids_buffer.size);
        const std::uint32_t bin_groups = (layout.bins + 63u) / 64u;
        if (!dispatch(bin_sort, bin_groups, control, job.generation, error))
            return false;

        if (resident) {
            resident->layout = layout;
            resident->contributing_particles = contributing_particles;
            resident->particle_buffer = std::move(particle_buffer);
            resident->counts_buffer = std::move(counts_buffer);
            resident->offsets_buffer = std::move(offsets_buffer);
            resident->ids_buffer = std::move(ids_buffer);
            return true;
        }

        GpuParticleBins candidate{};
        candidate.layout = layout;
        candidate.counts.resize(layout.bins);
        candidate.offsets.resize(layout.bins);
        candidate.particle_ids.resize(contributing_particles);
        if (!readback(vulkan, counts_buffer, candidate.counts.data(), bins_bytes,
                      error) ||
            !readback(vulkan, offsets_buffer, candidate.offsets.data(),
                      bins_bytes, error) ||
            (contributing_particles != 0u &&
             !readback(vulkan, ids_buffer, candidate.particle_ids.data(),
                       candidate.particle_ids.size() * sizeof(std::uint32_t),
                       error)))
            return false;
        bins = std::move(candidate);
        return true;
    }

    bool evaluate_field(const ParticleJob& job, std::vector<float>& values,
                        GridLayout& layout, Error& error,
                        const BuildControl& control = {},
                        ResidentField* resident = nullptr) {
        values.clear();
        layout = {};
        error = {};
        const auto bin_start = std::chrono::steady_clock::now();
        GpuParticleBins bins{};
        ResidentBins gpu_bins{};
        if (!build_bins(job, bins, error, control,
                        resident ? &gpu_bins : nullptr))
            return false;
        layout = resident ? gpu_bins.layout : bins.layout;
        const double bin_ms = elapsed_ms(bin_start);
        if (job.particle_count == 0u) return true;
        const auto field_start = std::chrono::steady_clock::now();

        matter::VkBufferResource params_buffer;
        matter::VkBufferResource particle_buffer;
        matter::VkBufferResource counts_buffer;
        matter::VkBufferResource offsets_buffer;
        matter::VkBufferResource ids_buffer;
        matter::VkBufferResource field_buffer;
        const std::size_t particles_bytes =
            static_cast<std::size_t>(job.particle_count) * sizeof(ParticleSample);
        const std::size_t bins_bytes =
            static_cast<std::size_t>(layout.bins) * sizeof(std::uint32_t);
        const std::uint32_t contributing_particles = resident
            ? gpu_bins.contributing_particles
            : static_cast<std::uint32_t>(bins.particle_ids.size());
        const std::size_t ids_bytes = std::max<std::size_t>(
            static_cast<std::size_t>(contributing_particles) *
                sizeof(std::uint32_t),
            sizeof(std::uint32_t));
        const std::size_t field_bytes =
            static_cast<std::size_t>(layout.grid_vertices) * sizeof(float);
        if (!create_gpu_buffer(vulkan, sizeof(FieldParams), params_buffer,
                               error) ||
            (!resident &&
             (!create_gpu_buffer(vulkan, particles_bytes, particle_buffer,
                                 error) ||
              !create_gpu_buffer(vulkan, bins_bytes, counts_buffer, error) ||
              !create_gpu_buffer(vulkan, bins_bytes, offsets_buffer, error) ||
              !create_gpu_buffer(vulkan, ids_bytes, ids_buffer, error))) ||
            !create_gpu_buffer(vulkan, field_bytes, field_buffer, error))
            return false;

        if (resident) {
            particle_buffer = std::move(gpu_bins.particle_buffer);
            counts_buffer = std::move(gpu_bins.counts_buffer);
            offsets_buffer = std::move(gpu_bins.offsets_buffer);
            ids_buffer = std::move(gpu_bins.ids_buffer);
        }

        const FieldParams params{
            {layout.origin_m.x, layout.origin_m.y, layout.origin_m.z,
             job.iso_value},
            {layout.spacing_m.x, layout.spacing_m.y, layout.spacing_m.z,
             job.blend_width_m},
            {layout.bin_origin_m.x, layout.bin_origin_m.y,
             layout.bin_origin_m.z, layout.bin_size_m},
            {layout.sample_dims[0], layout.sample_dims[1],
             layout.sample_dims[2], layout.grid_vertices},
            {layout.bin_dims[0], layout.bin_dims[1], layout.bin_dims[2],
             layout.bins},
            {job.particle_count, contributing_particles,
             resolved_particle_phase_split(job), 0u},
            {layout.query_radius_m, job.phase_blend.primary_weight,
             job.phase_blend.secondary_weight, 0.0f},
            {job.longitudinal_field_blend.source[0].primary_begin,
             job.longitudinal_field_blend.source[0].primary_count,
             job.longitudinal_field_blend.source[0].secondary_begin,
             job.longitudinal_field_blend.source[0].secondary_count},
            {job.longitudinal_field_blend.source[1].primary_begin,
             job.longitudinal_field_blend.source[1].primary_count,
             job.longitudinal_field_blend.source[1].secondary_begin,
             job.longitudinal_field_blend.source[1].secondary_count},
            {job.longitudinal_field_blend.origin_m.x,
             job.longitudinal_field_blend.origin_m.y,
             job.longitudinal_field_blend.origin_m.z,
             job.longitudinal_field_blend.enabled ? 1.0f : 0.0f},
            {job.longitudinal_field_blend.direction.x,
             job.longitudinal_field_blend.direction.y,
             job.longitudinal_field_blend.direction.z, 0.0f},
            {job.longitudinal_field_blend.upstream_full_m,
             job.longitudinal_field_blend.downstream_full_m, 0.0f, 0.0f},
        };
        if (!upload(vulkan, params_buffer, &params, sizeof(params), error) ||
            (!resident &&
             (!upload(vulkan, particle_buffer, job.particles,
                      particles_bytes, error) ||
              !upload(vulkan, counts_buffer, bins.counts.data(), bins_bytes,
                      error) ||
              !upload(vulkan, offsets_buffer, bins.offsets.data(), bins_bytes,
                      error) ||
              (!bins.particle_ids.empty() &&
               !upload(vulkan, ids_buffer, bins.particle_ids.data(),
                       bins.particle_ids.size() * sizeof(std::uint32_t),
                       error)))) ||
            !ensure_pipeline(vulkan, field, "gpu_mesh_field.comp.spv", 6u,
                             error))
            return false;
        matter::write_storage_buffer_descriptor(field, 0u, params_buffer, 0u,
                                                params_buffer.size);
        matter::write_storage_buffer_descriptor(field, 1u, particle_buffer, 0u,
                                                particle_buffer.size);
        matter::write_storage_buffer_descriptor(field, 2u, counts_buffer, 0u,
                                                counts_buffer.size);
        matter::write_storage_buffer_descriptor(field, 3u, offsets_buffer, 0u,
                                                offsets_buffer.size);
        matter::write_storage_buffer_descriptor(field, 4u, ids_buffer, 0u,
                                                ids_buffer.size);
        matter::write_storage_buffer_descriptor(field, 5u, field_buffer, 0u,
                                                field_buffer.size);
        const std::uint32_t groups =
            (layout.grid_vertices + 255u) / 256u;
        if (!dispatch(field, groups, control, job.generation, error))
            return false;

        if (resident) {
            resident->bins.layout = layout;
            resident->bins.contributing_particles = contributing_particles;
            resident->bins.particle_buffer = std::move(particle_buffer);
            resident->bins.counts_buffer = std::move(counts_buffer);
            resident->bins.offsets_buffer = std::move(offsets_buffer);
            resident->bins.ids_buffer = std::move(ids_buffer);
            resident->params = params;
            resident->params_buffer = std::move(params_buffer);
            resident->field_buffer = std::move(field_buffer);
            resident->bin_ms = bin_ms;
            resident->field_ms = elapsed_ms(field_start);
            return true;
        }

        std::vector<float> candidate(layout.grid_vertices);
        if (!readback(vulkan, field_buffer, candidate.data(), field_bytes,
                      error))
            return false;
        values = std::move(candidate);
        return true;
    }

    bool build_mesh(const ParticleJob& job, MeshResult& result, Stats& stats,
                    Error& error, const BuildControl& control) {
        result = {};
        stats = {};
        error = {};
        if (!check_control(control, job.generation, error)) return false;
        GridLayout layout{};
        if (!validate_particle_job(job, layout, error)) return false;
        stats.particles = job.particle_count;
        stats.bins = layout.bins;
        stats.grid_vertices = layout.grid_vertices;
        stats.grid_cells = layout.grid_cells;
        if (job.particle_count == 0u) {
            result.material = job.material;
            return true;
        }
        if (layout.grid_cells >
            std::numeric_limits<std::uint32_t>::max() / 5u) {
            return fail(error, ErrorCode::Overflow,
                        "GPU marching-cubes triangle count can overflow uint32");
        }

        std::vector<float> field_values;
        GridLayout field_layout{};
        ResidentField resident_field{};
        if (!evaluate_field(job, field_values, field_layout, error, control,
                            &resident_field) ||
            !check_control(control, job.generation, error))
            return false;
        stats.bin_ms = resident_field.bin_ms;
        stats.field_ms = resident_field.field_ms;
        const auto classify_start = std::chrono::steady_clock::now();

        std::array<std::int32_t, 256u * 16u> triangle_table{};
        for (std::size_t cube = 0; cube != 256u; ++cube) {
            std::size_t entries = 0u;
            bool terminated = false;
            for (std::size_t entry = 0; entry != 16u; ++entry) {
                const unsigned char raw =
                    static_cast<unsigned char>(triTable[cube][entry]);
                triangle_table[cube * 16u + entry] =
                    raw == 0xffu ? -1 : static_cast<std::int32_t>(raw);
                if (raw == 0xffu) {
                    terminated = true;
                } else if (!terminated) {
                    if (raw >= 12u)
                        return fail(error, ErrorCode::InvalidInput,
                                    "MatterSurface marching-cubes table has an invalid edge");
                    ++entries;
                } else {
                    return fail(error, ErrorCode::InvalidInput,
                                "MatterSurface marching-cubes table has entries after its terminator");
                }
            }
            if (!terminated || entries % 3u != 0u || entries > 15u)
                return fail(error, ErrorCode::InvalidInput,
                            "MatterSurface marching-cubes table row is malformed");
        }

        FieldParams params = resident_field.params;
        matter::VkBufferResource params_buffer =
            std::move(resident_field.params_buffer);
        matter::VkBufferResource field_buffer =
            std::move(resident_field.field_buffer);
        matter::VkBufferResource case_buffer;
        matter::VkBufferResource active_flags_buffer;
        matter::VkBufferResource triangle_counts_buffer;
        matter::VkBufferResource triangle_table_buffer;
        const std::size_t field_bytes =
            static_cast<std::size_t>(layout.grid_vertices) * sizeof(float);
        const std::size_t cells_bytes =
            static_cast<std::size_t>(layout.grid_cells) * sizeof(std::uint32_t);
        if (!create_gpu_buffer(vulkan, cells_bytes, case_buffer, error) ||
            !create_gpu_buffer(vulkan, cells_bytes, active_flags_buffer,
                               error) ||
            !create_gpu_buffer(vulkan, cells_bytes, triangle_counts_buffer,
                               error) ||
            !create_gpu_buffer(vulkan, sizeof(triangle_table),
                               triangle_table_buffer, error) ||
            !upload(vulkan, triangle_table_buffer, triangle_table.data(),
                    sizeof(triangle_table), error) ||
            !ensure_pipeline(vulkan, classify,
                             "gpu_mesh_classify.comp.spv", 6u, error))
            return false;
        matter::write_storage_buffer_descriptor(classify, 0u, params_buffer,
                                                0u, params_buffer.size);
        matter::write_storage_buffer_descriptor(classify, 1u, field_buffer,
                                                0u, field_buffer.size);
        matter::write_storage_buffer_descriptor(classify, 2u, case_buffer, 0u,
                                                case_buffer.size);
        matter::write_storage_buffer_descriptor(classify, 3u,
                                                active_flags_buffer, 0u,
                                                active_flags_buffer.size);
        matter::write_storage_buffer_descriptor(classify, 4u,
                                                triangle_counts_buffer, 0u,
                                                triangle_counts_buffer.size);
        matter::write_storage_buffer_descriptor(classify, 5u,
                                                triangle_table_buffer, 0u,
                                                triangle_table_buffer.size);
        const std::uint32_t cell_groups =
            (layout.grid_cells + 255u) / 256u;
        if (!dispatch(classify, cell_groups, control, job.generation, error))
            return false;

        matter::VkBufferResource active_offsets_buffer;
        matter::VkBufferResource triangle_offsets_buffer;
        std::uint32_t active_cells = 0;
        std::uint32_t triangles = 0;
        if (!scan_resource(active_flags_buffer, layout.grid_cells,
                           active_offsets_buffer, active_cells, error, control,
                           job.generation) ||
            !scan_resource(triangle_counts_buffer, layout.grid_cells,
                           triangle_offsets_buffer, triangles, error, control,
                           job.generation))
            return false;
        stats.active_cells = active_cells;
        stats.triangles = triangles;
        if (!check_control(control, job.generation, error)) return false;
        if (triangles == 0u) {
            result.material = job.material;
            result.content_digest = mesh_content_digest(result);
            return true;
        }
        const std::uint64_t vertex_count64 =
            static_cast<std::uint64_t>(triangles) * 3u;
        if (vertex_count64 > std::numeric_limits<std::uint32_t>::max())
            return fail(error, ErrorCode::Overflow,
                        "GPU marching-cubes output count overflows uint32");
        const std::uint32_t vertex_count =
            static_cast<std::uint32_t>(vertex_count64);
        if (vertex_count > job.limits.max_mesh_vertices ||
            vertex_count > job.limits.max_mesh_indices) {
            return fail(error, ErrorCode::LimitExceeded,
                        "GPU marching-cubes output exceeds declared capacity");
        }

        matter::VkBufferResource compact_params_buffer;
        matter::VkBufferResource active_cells_buffer;
        const ScanParams compact_params{layout.grid_cells, active_cells, 0u,
                                        0u};
        if (!create_gpu_buffer(vulkan, sizeof(compact_params),
                               compact_params_buffer, error) ||
            !create_gpu_buffer(vulkan,
                               static_cast<std::size_t>(active_cells) *
                                   sizeof(std::uint32_t),
                               active_cells_buffer, error) ||
            !upload(vulkan, compact_params_buffer, &compact_params,
                    sizeof(compact_params), error) ||
            !ensure_pipeline(vulkan, compact, "gpu_mesh_compact.comp.spv",
                             4u, error))
            return false;
        matter::write_storage_buffer_descriptor(compact, 0u,
                                                compact_params_buffer, 0u,
                                                compact_params_buffer.size);
        matter::write_storage_buffer_descriptor(compact, 1u,
                                                active_flags_buffer, 0u,
                                                active_flags_buffer.size);
        matter::write_storage_buffer_descriptor(compact, 2u,
                                                active_offsets_buffer, 0u,
                                                active_offsets_buffer.size);
        matter::write_storage_buffer_descriptor(compact, 3u,
                                                active_cells_buffer, 0u,
                                                active_cells_buffer.size);
        if (!dispatch(compact, cell_groups, control, job.generation, error))
            return false;
        stats.classify_ms = elapsed_ms(classify_start);

        if (!check_control(control, job.generation, error)) return false;
        const auto emit_start = std::chrono::steady_clock::now();
        matter::VkBufferResource particle_buffer =
            std::move(resident_field.bins.particle_buffer);
        matter::VkBufferResource bin_counts_buffer =
            std::move(resident_field.bins.counts_buffer);
        matter::VkBufferResource bin_offsets_buffer =
            std::move(resident_field.bins.offsets_buffer);
        matter::VkBufferResource particle_ids_buffer =
            std::move(resident_field.bins.ids_buffer);
        matter::VkBufferResource output_buffer;
        const std::size_t vec4_output_bytes =
            static_cast<std::size_t>(vertex_count) * sizeof(float) * 4u;
        const std::size_t index_bytes =
            static_cast<std::size_t>(vertex_count) * sizeof(std::uint32_t);
        const std::size_t position_offset = 0u;
        const std::size_t normal_offset = align_up_256(vec4_output_bytes);
        const std::size_t index_offset = align_up_256(
            normal_offset == 0u ? 0u : normal_offset + vec4_output_bytes);
        if (normal_offset == 0u || index_offset == 0u ||
            index_bytes > std::numeric_limits<std::size_t>::max() -
                              index_offset)
            return fail(error, ErrorCode::Overflow,
                        "GPU marching-cubes packed output size overflows");
        const std::size_t output_bytes = index_offset + index_bytes;
        if (!create_gpu_buffer(vulkan, output_bytes, output_buffer, error))
            return false;

        params.counts[3] = active_cells;
        if (!upload(vulkan, params_buffer, &params, sizeof(params), error) ||
            !ensure_pipeline(vulkan, emit, "gpu_mesh_emit.comp.spv", 13u,
                             error))
            return false;
        matter::VkBufferResource* emit_buffers[] = {
            &params_buffer,          &field_buffer,
            &case_buffer,            &active_cells_buffer,
            &triangle_offsets_buffer, &triangle_table_buffer,
            &particle_buffer,        &bin_counts_buffer,
            &bin_offsets_buffer,     &particle_ids_buffer,
        };
        for (std::uint32_t binding = 0u; binding != 10u; ++binding) {
            matter::write_storage_buffer_descriptor(
                emit, binding, *emit_buffers[binding], 0u,
                emit_buffers[binding]->size);
        }
        matter::write_storage_buffer_descriptor(
            emit, 10u, output_buffer,
            static_cast<VkDeviceSize>(position_offset),
            static_cast<VkDeviceSize>(vec4_output_bytes));
        matter::write_storage_buffer_descriptor(
            emit, 11u, output_buffer,
            static_cast<VkDeviceSize>(normal_offset),
            static_cast<VkDeviceSize>(vec4_output_bytes));
        matter::write_storage_buffer_descriptor(
            emit, 12u, output_buffer,
            static_cast<VkDeviceSize>(index_offset),
            static_cast<VkDeviceSize>(index_bytes));
        const std::uint32_t active_groups = (active_cells + 63u) / 64u;
        if (!dispatch(emit, active_groups, control, job.generation, error))
            return false;

        std::vector<std::array<float, 4>> packed_positions(vertex_count);
        std::vector<std::array<float, 4>> packed_normals(vertex_count);
        std::vector<std::uint32_t> indices(vertex_count);
        std::vector<std::uint8_t> packed_output(output_bytes);
        if (!readback(vulkan, output_buffer, packed_output.data(),
                      output_bytes, error) ||
            !check_control(control, job.generation, error))
            return false;
        std::memcpy(packed_positions.data(),
                    packed_output.data() + position_offset,
                    vec4_output_bytes);
        std::memcpy(packed_normals.data(),
                    packed_output.data() + normal_offset,
                    vec4_output_bytes);
        std::memcpy(indices.data(), packed_output.data() + index_offset,
                    index_bytes);

        MeshResult candidate{};
        candidate.positions.reserve(static_cast<std::size_t>(vertex_count) * 3u);
        candidate.normals.reserve(static_cast<std::size_t>(vertex_count) * 3u);
        candidate.indices = std::move(indices);
        candidate.material = job.material;
        for (std::uint32_t vertex = 0; vertex != vertex_count; ++vertex) {
            for (std::size_t axis = 0; axis != 3u; ++axis) {
                const float position = packed_positions[vertex][axis];
                const float normal = packed_normals[vertex][axis];
                if (!std::isfinite(position) || !std::isfinite(normal))
                    return fail(error, ErrorCode::VulkanFailure,
                                "GPU marching-cubes output is non-finite");
                candidate.positions.push_back(position);
                candidate.normals.push_back(normal);
            }
            if (candidate.indices[vertex] >= vertex_count)
                return fail(error, ErrorCode::VulkanFailure,
                            "GPU marching-cubes index is out of range");
        }
        candidate.content_digest = mesh_content_digest(candidate);
        stats.emit_ms = elapsed_ms(emit_start);
        stats.device_bytes =
            static_cast<std::uint64_t>(field_bytes) + cells_bytes * 5u +
            vec4_output_bytes * 2u + index_bytes;
        result = std::move(candidate);
        return true;
    }
};

GpuVisualMesher::GpuVisualMesher(matter::VulkanDevice& vulkan)
    : impl_(std::make_unique<Impl>(vulkan)) {}

GpuVisualMesher::~GpuVisualMesher() = default;

GpuMesherMemorySnapshot debug_gpu_mesher_memory_snapshot() {
    const matter::GpuMemoryStats stats = matter::gpu_memory_stats();
    return {stats.total_bytes, stats.allocation_count};
}

bool GpuVisualMesher::build_particle_visual(const ParticleJob& job,
                                            MeshResult& result, Stats& stats,
                                            Error& error,
                                            const BuildControl& control) {
    result = {};
    stats = {};
    error = {};
    try {
        return impl_->build_mesh(job, result, stats, error, control);
    } catch (const std::bad_alloc&) {
        result = {};
        stats = {};
        return fail(error, ErrorCode::Overflow,
                    "GPU visual mesh host allocation failed");
    } catch (...) {
        result = {};
        stats = {};
        return fail(error, ErrorCode::VulkanFailure,
                    "GPU visual mesh build failed unexpectedly");
    }
}

bool GpuVisualMesher::debug_exclusive_scan(
    const std::vector<std::uint32_t>& input,
    std::vector<std::uint32_t>& output, std::uint32_t& total, Error& error) {
    return impl_->scan_vector(input, output, total, error);
}

bool GpuVisualMesher::debug_build_particle_bins(const ParticleJob& job,
                                                GpuParticleBins& bins,
                                                Error& error) {
    return impl_->build_bins(job, bins, error);
}

bool GpuVisualMesher::debug_evaluate_particle_field(
    const ParticleJob& job, std::vector<float>& values, GridLayout& layout,
    Error& error) {
    return impl_->evaluate_field(job, values, layout, error);
}

}  // namespace gpu_meshing
