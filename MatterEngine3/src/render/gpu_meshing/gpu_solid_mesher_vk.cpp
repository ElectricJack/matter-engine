#include "gpu_solid_mesher_vk.h"
#include "render/vk_pipeline.h"
#include "render/vk_resources.h"
#include "render/vk_device_internal.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <new>
namespace gpu_meshing {
namespace {
#include "mc_tables.h"
using Clock = std::chrono::steady_clock;
double ms(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}
bool fail(Error &e, ErrorCode c, const std::string &m) {
    e = {c, m};
    return false;
}
bool current(const SolidJob &j, const BuildControl &c, Error &e) {
    if (c.cancelled && c.cancelled())
        return fail(e, ErrorCode::Cancelled, "solid job cancelled");
    if (c.generation_is_current && !c.generation_is_current(j.generation))
        return fail(e, ErrorCode::StaleGeneration, "solid generation stale");
    return true;
}
struct alignas(16) Params {
    float origin[4], spacing[4];
    std::uint32_t dims[4], limits[4];
};
struct Vertex {
    float p[4], n[4];
};
void barrier(VkCommandBuffer cmd, VkPipelineStageFlags src, VkAccessFlags sa,
             VkPipelineStageFlags dst, VkAccessFlags da) {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = sa;
    b.dstAccessMask = da;
    vkCmdPipelineBarrier(cmd, src, dst, 0, 1, &b, 0, nullptr, 0, nullptr);
}
} // namespace
struct GpuSolidMesher::Impl {
    matter::VulkanDevice &vk;
    matter::VkComputePipelineResource pipelines[4];
    matter::VkBufferResource buffers[8], readback;
    std::vector<Vertex> host_vertices;
    struct Queries final : matter::detail::DeviceLifetimeControl {
        explicit Queries(matter::VulkanDevice &v)
            : DeviceLifetimeControl(matter::detail::DeviceLifetimeAccess::token(v)) {}
        VkQueryPool pool = VK_NULL_HANDLE;
        ~Queries() override {
            release_device_objects();
        }
        void release_device_objects() noexcept override {
            auto d = live_device();
            if (d && pool)
                vkDestroyQueryPool(d, pool, nullptr);
            pool = VK_NULL_HANDLE;
        }
    };
    std::shared_ptr<Queries> queries;
    float period = 0;
    std::uint32_t timestamp_bits = 0;
    bool ready = false, poisoned = false;
    GridLayout layout{};
    std::uint32_t output_capacity = 0;
    explicit Impl(matter::VulkanDevice &v) : vk(v) {}
    bool ensure(matter::VkBufferResource &b, VkDeviceSize bytes, bool host,
                std::string &e, bool cached_readback = false) {
        if (b.size >= bytes)
            return true;
        VkDeviceSize capacity = std::max(bytes, b.size + b.size / 2);
        capacity = (capacity + 255) & ~VkDeviceSize(255);
        return matter::create_buffer(
            vk, capacity,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                 : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            host ? (cached_readback ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT
                                    : VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                 : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            b, e);
    }
    bool initialize(std::string &e) {
        if (ready)
            return true;
        if (vk.device() == VK_NULL_HANDLE) {
            e = "Vulkan solid mesher unavailable";
            return false;
        }
        const char *names[] = {"solid_field.comp.spv", "solid_classify.comp.spv",
                               "solid_scan.comp.spv", "solid_emit.comp.spv"};
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        for (std::uint32_t i = 0; i < 8; ++i) {
            VkDescriptorSetLayoutBinding b{};
            b.binding = i;
            b.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            b.descriptorCount = 1;
            b.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            bindings.push_back(b);
        }
        for (int i = 0; i < 4; ++i)
            if (!matter::create_compute_pipeline(vk, names[i], bindings, pipelines[i],
                                                 e))
                return false;
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(vk.physical_device(), &props);
        period = props.limits.timestampPeriod;
        std::uint32_t n = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(vk.physical_device(), &n, nullptr);
        std::vector<VkQueueFamilyProperties> families(n);
        vkGetPhysicalDeviceQueueFamilyProperties(vk.physical_device(), &n,
                                                 families.data());
        timestamp_bits = families[vk.graphics_queue_family()].timestampValidBits;
        if (timestamp_bits) {
            VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            info.queryType = VK_QUERY_TYPE_TIMESTAMP;
            info.queryCount = 2;
            VkQueryPool pool{};
            VkResult r = vkCreateQueryPool(vk.device(), &info, nullptr, &pool);
            if (r != VK_SUCCESS) {
                e = "solid timestamp pool creation failed";
                return false;
            }
            queries = std::make_shared<Queries>(vk);
            queries->pool = pool;
        }
        ready = true;
        return true;
    }
    bool prepare(const SolidJob &j, std::string &e) {
        if (!initialize(e))
            return false;
        // Output allocation/copy is bounded by both caller capacity and worst case.
        output_capacity = std::min(j.max_mesh_vertices, layout.grid_cells * 15u);
        VkDeviceSize sizes[] = {sizeof(Params),
                                j.op_count * sizeof(SolidOp),
                                layout.grid_vertices * sizeof(float),
                                layout.grid_cells * 8ull,
                                ((layout.grid_cells + 255ull) / 256) * 4,
                                256 * 16 * 4,
                                16,
                                output_capacity * sizeof(Vertex)};
        for (int i = 0; i < 8; ++i)
            if (!ensure(buffers[i], sizes[i], i == 0 || i == 1 || i == 5, e))
                return false;
        bool prefer_cached = true;
#ifdef MATTER_VK_TEST_FAULT_INJECTION
        prefer_cached = std::getenv("MATTER_SOLID_TEST_COHERENT_READBACK") == nullptr;
#endif
        if (!ensure(readback,
                    16 + std::max(VkDeviceSize(output_capacity) * sizeof(Vertex),
                                  VkDeviceSize(layout.grid_vertices) * 4),
                    true, e, prefer_cached))
            return false;
        Params p{};
        p.origin[0] = layout.origin_m.x;
        p.origin[1] = layout.origin_m.y;
        p.origin[2] = layout.origin_m.z;
        p.spacing[0] = p.spacing[1] = p.spacing[2] = j.voxel_m;
        p.spacing[3] = j.voxel_m * .02f;
        for (int i = 0; i < 3; ++i)
            p.dims[i] = layout.sample_dims[i];
        p.dims[3] = layout.grid_vertices;
        p.limits[0] = j.op_count;
        p.limits[1] = layout.grid_cells;
        p.limits[2] = output_capacity;
        std::int32_t table[256 * 16];
        for (int i = 0; i < 256; ++i)
            for (int k = 0; k < 16; ++k)
                table[i * 16 + k] = static_cast<signed char>(triTable[i][k]);
        if (!matter::upload_buffer(vk, buffers[0], &p, sizeof(p), 0, e) ||
            !matter::upload_buffer(vk, buffers[1], j.ops, j.op_count * sizeof(SolidOp),
                                   0, e) ||
            !matter::upload_buffer(vk, buffers[5], table, sizeof(table), 0, e))
            return false;
        for (auto &pipeline : pipelines)
            for (std::uint32_t i = 0; i < 8; ++i)
                matter::write_storage_buffer_descriptor(pipeline, i, buffers[i], 0,
                                                        sizes[i]);
        return true;
    }
    struct Record {
        Impl *self;
        bool field_only;
    };
    static void record(VkCommandBuffer cmd, void *user) {
        auto &r = *static_cast<Record *>(user);
        auto &s = *r.self;
        barrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_WRITE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        if (s.queries) {
            vkCmdResetQueryPool(cmd, s.queries->pool, 0, 2);
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, s.queries->pool,
                                0);
        }
        for (int i = 0; i < (r.field_only ? 1 : 4); ++i) {
            auto &p = s.pipelines[i];
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    p.pipeline_layout, 0, 1, &p.descriptor_set, 0,
                                    nullptr);
            std::uint32_t groups = i == 0   ? (s.layout.grid_vertices + 255u) / 256u
                                   : i == 2 ? 1u
                                            : (s.layout.grid_cells + 255u) / 256u;
            vkCmdDispatch(cmd, groups, 1, 1);
            barrier(
                cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                    VK_ACCESS_TRANSFER_READ_BIT);
        }
        if (s.queries)
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                s.queries->pool, 1);
        if (r.field_only) {
            VkBufferCopy c{0, 0, VkDeviceSize(s.layout.grid_vertices) * 4};
            vkCmdCopyBuffer(cmd, s.buffers[2].buffer, s.readback.buffer, 1, &c);
        } else {
            VkBufferCopy a{0, 0, 16},
                b{0, 16, VkDeviceSize(s.output_capacity) * sizeof(Vertex)};
            vkCmdCopyBuffer(cmd, s.buffers[6].buffer, s.readback.buffer, 1, &a);
            vkCmdCopyBuffer(cmd, s.buffers[7].buffer, s.readback.buffer, 1, &b);
        }
        barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    }
    bool submit(bool field_only, Error &e) {
        std::vector<std::shared_ptr<void>> keep;
        for (auto &b : buffers)
            keep.push_back(b.lifetime);
        keep.push_back(readback.lifetime);
        for (auto &p : pipelines)
            keep.push_back(p.lifetime);
        if (queries)
            keep.push_back(queries);
        Record r{this, field_only};
        std::string message;
        if (!matter::submit_immediate(vk, record, &r, message,
                                      matter::ImmediateSubmitPhase::compute_dispatch,
                                      std::move(keep))) {
            poisoned = true;
            return fail(e,
                        message.find("VkResult -4") != std::string::npos
                            ? ErrorCode::DeviceLost
                            : ErrorCode::VulkanFailure,
                        message);
        }
        if (!matter::map_buffer(readback, message) ||
            !matter::invalidate_buffer(readback, 0, readback.size, message))
            return fail(e, ErrorCode::VulkanFailure, message);
        return true;
    }
};
GpuSolidMesher::GpuSolidMesher(matter::VulkanDevice &v)
    : impl_(std::make_unique<Impl>(v)) {}
GpuSolidMesher::~GpuSolidMesher() = default;
bool GpuSolidMesher::build(const SolidJob &j, MeshResult &out, SolidStats &stats,
                           Error &e, const BuildControl &control) {
    auto start = Clock::now();
    stats = {};
    e = {};
    auto &s = *impl_;
    if (!current(j, control, e) || !validate_solid_job(j, s.layout, e))
        return false;
    if (s.poisoned)
        return fail(e, ErrorCode::VulkanFailure,
                    "solid service unusable after failed submission");
    if (s.vk.device() == VK_NULL_HANDLE)
        return fail(e, ErrorCode::Unavailable, "Vulkan solid mesher unavailable");
    try {
        std::string message;
        if (!s.prepare(j, message))
            return fail(e, ErrorCode::VulkanFailure, message);
        stats.prepare_ms = ms(start);
        stats.layout = s.layout;
        stats.recipe_digest = solid_recipe_digest(j);
        if (!current(j, control, e))
            return false;
        auto submit = Clock::now();
        if (!s.submit(false, e))
            return false;
        stats.submit_wait_ms = ms(submit);
        stats.submissions = 1;
        if (!current(j, control, e))
            return false;
        std::uint32_t status[4];
        std::memcpy(status, s.readback.mapped, 16);
        if (status[1] || status[0] > s.output_capacity)
            return fail(e, ErrorCode::Overflow,
                        "solid output capacity exceeded; no partial mesh returned");
        auto decode = Clock::now();
        MeshResult result;
        result.material = j.material;
        std::uint32_t n = status[0];
        result.positions.resize(std::size_t(n) * 3);
        result.normals.resize(std::size_t(n) * 3);
        result.indices.resize(n);
        const auto *v = reinterpret_cast<const Vertex *>(
            static_cast<const char *>(s.readback.mapped) + 16);
        stats.readback_memory_flags = s.readback.memory_properties;
        if (!(s.readback.memory_properties & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) {
            // Never repeatedly dereference uncached/BAR mapped memory. A bulk
            // copy into persistent ordinary RAM amortizes bus/cache-line reads.
            auto copy_start = Clock::now();
            s.host_vertices.resize(n);
            if (n)
                std::memcpy(s.host_vertices.data(), v, std::size_t(n) * sizeof(Vertex));
            v = s.host_vertices.data();
            stats.readback_copy_ms = ms(copy_start);
        }
        stats.host_scratch_bytes = s.host_vertices.capacity() * sizeof(Vertex);
        for (std::uint32_t i = 0; i < n; ++i) {
            for (int k = 0; k < 3; ++k) {
                if (!std::isfinite(v[i].p[k]) || !std::isfinite(v[i].n[k]))
                    return fail(e, ErrorCode::ArtifactFailure,
                                "nonfinite solid output");
                result.positions[3ull * i + k] = v[i].p[k];
                result.normals[3ull * i + k] = v[i].n[k];
            }
            result.indices[i] = i;
        }
        stats.decode_ms = ms(decode);
        auto digest_start = Clock::now();
        result.content_digest = mesh_content_digest(result);
        stats.digest_ms = ms(digest_start);
        stats.vertices = n;
        stats.gpu_ms = std::numeric_limits<double>::quiet_NaN();
        if (s.queries) {
            std::uint64_t ts[2];
            if (vkGetQueryPoolResults(s.vk.device(), s.queries->pool, 0, 2, sizeof(ts),
                                      ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
                auto mask = s.timestamp_bits == 64
                                ? ~std::uint64_t(0)
                                : (std::uint64_t(1) << s.timestamp_bits) - 1;
                stats.gpu_ms = double((ts[1] - ts[0]) & mask) * s.period / 1e6;
            }
        }
        for (auto &b : s.buffers)
            stats.resident_bytes += b.allocation_size;
        stats.resident_bytes += s.readback.allocation_size;
        if (!current(j, control, e))
            return false;
        out = std::move(result);
        stats.host_ms = ms(start);
        return true;
    } catch (const std::bad_alloc &) {
        return fail(e, ErrorCode::LimitExceeded, "solid host allocation failed");
    }
}
bool GpuSolidMesher::debug_field(const SolidJob &j, std::vector<float> &out,
                                 GridLayout &layout, Error &e) {
    auto &s = *impl_;
    if (!validate_solid_job(j, s.layout, e))
        return false;
    if (s.poisoned)
        return fail(e, ErrorCode::VulkanFailure, "solid service poisoned");
    std::string m;
    if (!s.prepare(j, m))
        return fail(e, ErrorCode::VulkanFailure, m);
    if (!s.submit(true, e))
        return false;
    std::vector<float> result(s.layout.grid_vertices);
    std::memcpy(result.data(), s.readback.mapped, result.size() * 4);
    out = std::move(result);
    layout = s.layout;
    return true;
}
} // namespace gpu_meshing
