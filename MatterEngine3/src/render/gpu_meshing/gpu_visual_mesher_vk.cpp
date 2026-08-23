#include "gpu_visual_mesher_vk.h"

#include "render/vk_pipeline.h"
#include "render/vk_resources.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace gpu_meshing {
namespace {

constexpr std::uint32_t kScanWorkgroup = 256u;

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

static_assert(sizeof(ScanParams) == 16, "scan params must match one uvec4");
static_assert(sizeof(BinParams) == 48,
              "bin params must match three std430 vec4 values");
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

    bool dispatch(matter::VkComputePipelineResource& pipeline,
                  std::uint32_t x, const BuildControl& control,
                  std::uint64_t generation, Error& error) {
        if (!check_control(control, generation, error)) return false;
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
                    Error& error) {
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
            {job.particle_count, 0u, 0u, 0u},
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
        if (!dispatch(bin_count, particle_groups, {}, job.generation, error))
            return false;

        std::uint32_t contributing_particles = 0;
        if (!scan_resource(counts_buffer, layout.bins, offsets_buffer,
                           contributing_particles, error, {}, job.generation))
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
        if (!dispatch(bin_scatter, particle_groups, {}, job.generation, error) ||
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
        if (!dispatch(bin_sort, bin_groups, {}, job.generation, error))
            return false;

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
};

GpuVisualMesher::GpuVisualMesher(matter::VulkanDevice& vulkan)
    : impl_(std::make_unique<Impl>(vulkan)) {}

GpuVisualMesher::~GpuVisualMesher() = default;

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

}  // namespace gpu_meshing
