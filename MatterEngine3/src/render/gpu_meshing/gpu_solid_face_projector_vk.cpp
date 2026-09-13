#include "gpu_solid_face_projector_vk.h"
#include "render/vk_device_internal.h"
#include "render/vk_pipeline.h"
#include "render/vk_resources.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>

namespace gpu_meshing {
namespace {
using Clock = std::chrono::steady_clock;
double ms(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}
bool fail(Error &e, ErrorCode c, const std::string &s) {
    e = {c, s};
    return false;
}
bool current(const FaceJob &j, const BuildControl &c, Error &e) {
    if (c.cancelled && c.cancelled())
        return fail(e, ErrorCode::Cancelled, "face projection cancelled");
    if (c.generation_is_current && !c.generation_is_current(j.source.generation))
        return fail(e, ErrorCode::StaleGeneration, "face projection generation stale");
    return true;
}
struct alignas(16) Params {
    float origin[4], spacing[4];
    std::uint32_t dims[4], limits[4];
};
struct alignas(16) Projection {
    float origin[4], u[4], v[4], n[4], rect[4], interval[4], bounds_min[4], bounds_max[4];
    std::uint32_t limits[4];
};
struct Pixel {
    float normal_height[4];
    std::uint32_t status[4];
};
static_assert(sizeof(Pixel) == 32 && sizeof(Projection) == 144, "projection GPU ABI");
void vector(float *out, matter::Float3 v) {
    out[0] = v.x;
    out[1] = v.y;
    out[2] = v.z;
}
void barrier(VkCommandBuffer cmd, VkPipelineStageFlags src, VkAccessFlags sa,
             VkPipelineStageFlags dst, VkAccessFlags da) {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = sa;
    b.dstAccessMask = da;
    vkCmdPipelineBarrier(cmd, src, dst, 0, 1, &b, 0, nullptr, 0, nullptr);
}
} // namespace
struct GpuSolidFaceProjector::Impl {
    matter::VulkanDevice &vk;
    matter::VkComputePipelineResource pipeline;
    matter::VkBufferResource buffers[4], readback;
    std::vector<Pixel> scratch;
    struct Queries final : matter::detail::DeviceLifetimeControl {
        explicit Queries(matter::VulkanDevice &v)
            : DeviceLifetimeControl(matter::detail::DeviceLifetimeAccess::token(v)) {}
        VkQueryPool pool = VK_NULL_HANDLE;
        ~Queries() override { release_device_objects(); }
        void release_device_objects() noexcept override {
            auto d = live_device();
            if (d && pool)
                vkDestroyQueryPool(d, pool, nullptr);
            pool = VK_NULL_HANDLE;
        }
    };
    std::shared_ptr<Queries> queries;
    float timestamp_period = 0;
    std::uint32_t timestamp_bits = 0, count = 0;
    bool ready = false, poisoned = false;
    explicit Impl(matter::VulkanDevice &v) : vk(v) {}
    bool ensure(matter::VkBufferResource &b, VkDeviceSize bytes, bool host, bool cached,
                std::string &e) {
        if (b.size >= bytes)
            return true;
        VkDeviceSize capacity = (std::max(bytes, b.size + b.size / 2) + 255) & ~VkDeviceSize(255);
        return matter::create_buffer(
            vk, capacity,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            host ? (cached ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT
                           : VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                 : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            b, e);
    }
    bool initialize(std::string &e) {
        if (ready)
            return true;
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        for (std::uint32_t i = 0; i < 4; ++i) {
            VkDescriptorSetLayoutBinding b{};
            b.binding = i;
            b.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            b.descriptorCount = 1;
            b.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            bindings.push_back(b);
        }
        if (!matter::create_compute_pipeline(vk, "solid_face_project.comp.spv", bindings, pipeline,
                                             e))
            return false;
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(vk.physical_device(), &props);
        timestamp_period = props.limits.timestampPeriod;
        std::uint32_t n = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(vk.physical_device(), &n, nullptr);
        std::vector<VkQueueFamilyProperties> families(n);
        vkGetPhysicalDeviceQueueFamilyProperties(vk.physical_device(), &n, families.data());
        timestamp_bits = families[vk.graphics_queue_family()].timestampValidBits;
        if (timestamp_bits) {
            VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            info.queryType = VK_QUERY_TYPE_TIMESTAMP;
            info.queryCount = 2;
            auto q = std::make_shared<Queries>(vk);
            if (vkCreateQueryPool(vk.device(), &info, nullptr, &q->pool) != VK_SUCCESS) {
                e = "face timestamp pool creation failed";
                return false;
            }
            queries = std::move(q);
        }
        ready = true;
        return true;
    }
    bool prepare(const FaceJob &j, const FaceLayout &l, std::string &e) {
        if (!initialize(e))
            return false;
        count = l.width * l.height;
        VkDeviceSize sizes[] = {sizeof(Params), j.source.op_count * sizeof(SolidOp),
                                sizeof(Projection), count * sizeof(Pixel)};
        for (int i = 0; i < 4; ++i)
            if (!ensure(buffers[i], sizes[i], i != 3, false, e))
                return false;
        if (!ensure(readback, sizes[3], true, true, e))
            return false;
        Params p{};
        p.spacing[3] = j.normal_epsilon_m;
        p.limits[0] = j.source.op_count;
        Projection f{};
        vector(f.origin, j.frame.origin_m);
        vector(f.u, j.frame.u);
        vector(f.v, j.frame.v);
        vector(f.n, j.frame.n);
        vector(f.bounds_min, l.source_bounds.min_m);
        vector(f.bounds_max, l.source_bounds.max_m);
        f.rect[0] = j.u_min_m;
        f.rect[1] = j.v_min_m;
        f.rect[2] = l.pitch_u_m;
        f.rect[3] = l.pitch_v_m;
        f.interval[0] = j.height_min_m;
        f.interval[1] = j.height_max_m;
        f.interval[2] = j.hit_epsilon_m;
        bool prove_base = j.source.ops[0].kind[0] <= 2;
        for (std::uint32_t i = 1; i < j.source.op_count; ++i)
            if (j.source.ops[i].kind[1] == 0) prove_base = false;
        f.interval[3] = prove_base ? 1.f : 0.f;
        f.limits[0] = l.width;
        f.limits[1] = l.height;
        f.limits[2] = j.max_steps;
        f.limits[3] = j.refine_steps;
        if (!matter::upload_buffer(vk, buffers[0], &p, sizeof(p), 0, e) ||
            !matter::upload_buffer(vk, buffers[1], j.source.ops, sizes[1], 0, e) ||
            !matter::upload_buffer(vk, buffers[2], &f, sizeof(f), 0, e))
            return false;
        for (std::uint32_t i = 0; i < 4; ++i)
            matter::write_storage_buffer_descriptor(pipeline, i, buffers[i], 0, sizes[i]);
        return true;
    }
    static void record(VkCommandBuffer cmd, void *user) {
        auto &s = *static_cast<Impl *>(user);
        barrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_WRITE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        if (s.queries) {
            vkCmdResetQueryPool(cmd, s.queries->pool, 0, 2);
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, s.queries->pool, 0);
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.pipeline.pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.pipeline.pipeline_layout, 0,
                                1, &s.pipeline.descriptor_set, 0, nullptr);
        vkCmdDispatch(cmd, (s.count + 127) / 128, 1, 1);
        if (s.queries)
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, s.queries->pool, 1);
        barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferCopy copy{0, 0, s.count * sizeof(Pixel)};
        vkCmdCopyBuffer(cmd, s.buffers[3].buffer, s.readback.buffer, 1, &copy);
        barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    }
    bool submit(Error &e) {
        std::vector<std::shared_ptr<void>> keep;
        for (auto &b : buffers)
            keep.push_back(b.lifetime);
        keep.push_back(readback.lifetime);
        keep.push_back(pipeline.lifetime);
        if (queries)
            keep.push_back(queries);
        std::string message;
        if (!matter::submit_immediate(vk, record, this, message,
                                      matter::ImmediateSubmitPhase::compute_dispatch,
                                      std::move(keep))) {
            poisoned = true;
            return fail(e,
                        message.find("VkResult -4") != std::string::npos ? ErrorCode::DeviceLost
                                                                         : ErrorCode::VulkanFailure,
                        message);
        }
        if (!matter::map_buffer(readback, message) ||
            !matter::invalidate_buffer(readback, 0, readback.size, message))
            return fail(e, ErrorCode::VulkanFailure, message);
        return true;
    }
};
GpuSolidFaceProjector::GpuSolidFaceProjector(matter::VulkanDevice &v)
    : impl_(std::make_unique<Impl>(v)) {}
GpuSolidFaceProjector::~GpuSolidFaceProjector() = default;
bool GpuSolidFaceProjector::project(const FaceJob &j, FacePatch &out, FaceStats &stats, Error &e,
                                    const BuildControl &control) {
    auto start = Clock::now();
    stats = {};
    e = {};
    FaceLayout l;
    auto &s = *impl_;
    if (!current(j, control, e) || !validate_face_job(j, l, e))
        return false;
    if (s.poisoned)
        return fail(e, ErrorCode::VulkanFailure, "face service unusable after failed submission");
    if (s.vk.device() == VK_NULL_HANDLE)
        return fail(e, ErrorCode::Unavailable, "Vulkan face service unavailable");
    try {
        std::string message;
        if (!s.prepare(j, l, message))
            return fail(e, ErrorCode::VulkanFailure, message);
        stats.prepare_ms = ms(start);
        if (!current(j, control, e))
            return false;
        auto submit = Clock::now();
        if (!s.submit(e))
            return false;
        stats.submit_wait_ms = ms(submit);
        stats.submissions = 1;
        if (!current(j, control, e))
            return false;
        auto decode = Clock::now();
        const auto *pixels = static_cast<const Pixel *>(s.readback.mapped);
        stats.readback_memory_flags = s.readback.memory_properties;
        if (!(s.readback.memory_properties & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) {
            auto copy = Clock::now();
            s.scratch.resize(s.count);
            std::memcpy(s.scratch.data(), pixels, s.count * sizeof(Pixel));
            pixels = s.scratch.data();
            stats.readback_copy_ms = ms(copy);
        }
        stats.host_scratch_bytes = s.scratch.capacity() * sizeof(Pixel);
        FacePatch result;
        result.frame = j.frame;
        result.layout = l;
        result.u_min_m = j.u_min_m;
        result.u_max_m = j.u_max_m;
        result.v_min_m = j.v_min_m;
        result.v_max_m = j.v_max_m;
        result.height_min_m = j.height_min_m;
        result.height_max_m = j.height_max_m;
        result.material = j.source.material;
        result.recipe_digest = face_recipe_digest(j);
        result.texels.resize(s.count);
        for (std::uint32_t i = 0; i < s.count; ++i) {
            const auto &p = pixels[i];
            if (p.status[1])
                return fail(
                    e, p.status[1] == 1 ? ErrorCode::LimitExceeded : ErrorCode::ArtifactFailure,
                    "projected ray failed (status " + std::to_string(p.status[1]) +
                        ") pixel=" + std::to_string(i) + " xy=" +
                        std::to_string(i % l.width) + "," + std::to_string(i / l.width) +
                        " uv_m=" + std::to_string(j.u_min_m + (i % l.width + .5f)*l.pitch_u_m) +
                        "," + std::to_string(j.v_min_m + (i / l.width + .5f)*l.pitch_v_m) +
                        " height_m=" + std::to_string(p.normal_height[3]) +
                        " field_m=" + std::to_string(p.normal_height[0]) +
                        " steps=" + std::to_string(p.status[2]) + "; no partial patch");
            if (p.status[0] > 1 || p.status[3] || p.status[2] > j.max_steps)
                return fail(e, ErrorCode::ArtifactFailure, "invalid face GPU status");
            auto &t = result.texels[i];
            t.coverage = p.status[0];
            stats.max_steps_used = std::max(stats.max_steps_used, p.status[2]);
            for (float f : p.normal_height)
                if (!std::isfinite(f))
                    return fail(e, ErrorCode::ArtifactFailure, "nonfinite face GPU result");
            if (t.coverage) {
                t.height_m = p.normal_height[3];
                t.normal_uvn = {p.normal_height[0], p.normal_height[1], p.normal_height[2]};
                float norm = p.normal_height[0] * p.normal_height[0] +
                             p.normal_height[1] * p.normal_height[1] +
                             p.normal_height[2] * p.normal_height[2];
                if (std::abs(norm - 1) > 1e-3f || t.height_m < j.height_min_m ||
                    t.height_m > j.height_max_m)
                    return fail(e, ErrorCode::ArtifactFailure,
                                "invalid projected height or normal");
                ++stats.covered_pixels;
            }
        }
        stats.decode_ms = ms(decode);
        stats.gpu_ms = std::numeric_limits<double>::quiet_NaN();
        if (s.queries) {
            std::uint64_t ts[2];
            if (vkGetQueryPoolResults(s.vk.device(), s.queries->pool, 0, 2, sizeof(ts), ts, 8,
                                      VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
                auto mask = s.timestamp_bits == 64 ? ~std::uint64_t(0)
                                                   : (std::uint64_t(1) << s.timestamp_bits) - 1;
                stats.gpu_ms = double((ts[1] - ts[0]) & mask) * s.timestamp_period / 1e6;
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
        return fail(e, ErrorCode::LimitExceeded, "face host allocation failed");
    }
}
} // namespace gpu_meshing
