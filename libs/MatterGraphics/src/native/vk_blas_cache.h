#pragma once
#include "blas_disk_cache.h"
#include "vk_resources.h"
#include "matter/vulkan_device.h"
#include "matter/log.h"
#include <cstring>

namespace viewer {
// Static triangle BLAS only: no micromaps, TLAS addresses or animation state.
// GPU captures are consumed only when their frame slot's fence has retired.
class VkBlasCache {
public:
    explicit VkBlasCache(matter::VulkanDevice& vulkan) : vulkan_(vulkan) {
        VkPhysicalDeviceIDProperties ids{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props.pNext = &ids; vkGetPhysicalDeviceProperties2(vulkan.physical_device(), &props);
        fingerprint_ = asset_store::hash_to_string(asset_store::hash_bytes(ids.driverUUID, VK_UUID_SIZE));
        fingerprint_ += asset_store::hash_to_string(asset_store::hash_bytes(ids.deviceUUID, VK_UUID_SIZE));
        serialize_ = reinterpret_cast<PFN_vkCmdCopyAccelerationStructureToMemoryKHR>(vkGetDeviceProcAddr(vulkan.device(), "vkCmdCopyAccelerationStructureToMemoryKHR"));
        deserialize_ = reinterpret_cast<PFN_vkCmdCopyMemoryToAccelerationStructureKHR>(vkGetDeviceProcAddr(vulkan.device(), "vkCmdCopyMemoryToAccelerationStructureKHR"));
        compatibility_ = reinterpret_cast<PFN_vkGetDeviceAccelerationStructureCompatibilityKHR>(vkGetDeviceProcAddr(vulkan.device(), "vkGetDeviceAccelerationStructureCompatibilityKHR"));
        query_ = reinterpret_cast<PFN_vkCmdWriteAccelerationStructuresPropertiesKHR>(vkGetDeviceProcAddr(vulkan.device(), "vkCmdWriteAccelerationStructuresPropertiesKHR"));
    }
    std::string key(const std::string& content, bool opaque) const {
        return "triangle-blas-v1/" + fingerprint_ + "/" + content + (opaque ? "/opaque" : "/anyhit");
    }
    bool available() const { return serialize_ && deserialize_ && compatibility_ && query_; }
    BlasDiskCache disk;
    uint64_t hits = 0, misses = 0, captures = 0, rejected = 0;
    bool restore(const matter::VulkanFrame& frame, const BlasDiskCache::Bytes& bytes,
                 const matter::VkAccelerationStructureResource& target, VkDeviceSize capacity,
                 std::string& error) {
        if (!available() || !bytes || bytes->size() < 48) return false;
        uint64_t serialized = 0, deserialized = 0;
        std::memcpy(&serialized, bytes->data() + 32, 8);
        std::memcpy(&deserialized, bytes->data() + 40, 8);
        VkAccelerationStructureVersionInfoKHR version{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_VERSION_INFO_KHR};
        version.pVersionData = bytes->data();
        VkAccelerationStructureCompatibilityKHR compatible;
        compatibility_(vulkan_.device(), &version, &compatible);
        if (serialized != bytes->size() || !deserialized || deserialized > capacity ||
            compatible != VK_ACCELERATION_STRUCTURE_COMPATIBILITY_COMPATIBLE_KHR) { ++rejected; return false; }
        matter::VkBufferResource buffer;
        if (!host_buffer(bytes->size(), buffer, error)) return false;
        const auto offset = aligned_offset(buffer);
        std::memcpy(static_cast<uint8_t*>(buffer.mapped) + offset, bytes->data(), bytes->size());
        if (!matter::flush_buffer(buffer, offset, bytes->size(), error)) return false;
        if (!vulkan_.retain_for_frame(frame, {buffer.lifetime, target.lifetime}, error)) return false;
        VkCopyMemoryToAccelerationStructureInfoKHR copy{VK_STRUCTURE_TYPE_COPY_MEMORY_TO_ACCELERATION_STRUCTURE_INFO_KHR};
        copy.src.deviceAddress = buffer.address + offset; copy.dst = target.handle;
        copy.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_DESERIALIZE_KHR;
        deserialize_(frame.command_buffer, &copy);
        ++hits; return true;
    }
    void capture(const matter::VulkanFrame& frame, std::string directory, std::string key,
                 std::shared_ptr<matter::VkAccelerationStructureResource> blas,
                 std::shared_ptr<const void> reservation) {
        // A slot can hold last rotation's readbacks and this rotation's
        // queries together; each geometry warmup queue admits up to 128 pages.
        if (!available() || pending_[frame.frame_slot].size() >= 256) return;
        auto query = std::make_shared<Query>(); query->device = vulkan_.device();
        VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        info.queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_SERIALIZATION_SIZE_KHR; info.queryCount = 1;
        if (vkCreateQueryPool(query->device, &info, nullptr, &query->pool) != VK_SUCCESS) return;
        std::string error;
        if (!vulkan_.retain_for_frame(frame, {query, blas->lifetime}, error)) return;
        vkCmdResetQueryPool(frame.command_buffer, query->pool, 0, 1);
        query_(frame.command_buffer, 1, &blas->handle, info.queryType, query->pool, 0);
        pending_[frame.frame_slot].push_back({frame.serial, false, std::move(directory), std::move(key), std::move(blas), std::move(query), {}, 0, std::move(reservation)});
    }
    void finish(uint64_t serial, bool succeeded) {
        for (auto& slot : pending_) for (auto& item : slot.second)
            if (item.serial == serial) item.submitted = succeeded;
    }
    void begin(const matter::VulkanFrame& frame) {
        auto& items = pending_[frame.frame_slot];
        for (auto it = items.begin(); it != items.end();) {
            auto& item = *it; std::string error;
            if (!item.submitted) { it = items.erase(it); continue; }
            if (item.bytes) {
                const auto offset = aligned_offset(*item.bytes);
                if (!item.ready_bytes) {
                    if (!matter::invalidate_buffer(*item.bytes, offset, item.size, error)) {
                        it = items.erase(it); continue;
                    }
                    const auto* data = static_cast<const uint8_t*>(item.bytes->mapped) + offset;
                    item.ready_bytes = std::make_shared<const std::vector<uint8_t>>(data, data + item.size);
                }
                // Writer saturation is backpressure. Retain the immutable
                // payload and reservation until accepted, without recopying.
                if (!disk.write(item.directory, item.key, item.ready_bytes)) { ++it; continue; }
                ++captures;
                it = items.erase(it); continue;
            }
            uint64_t size = 0;
            const auto status = vkGetQueryPoolResults(vulkan_.device(), item.query->pool, 0, 1,
                sizeof(size), &size, sizeof(size), VK_QUERY_RESULT_64_BIT);
            if (status != VK_SUCCESS || size < 48 || size > BlasDiskCache::max_blob) { it = items.erase(it); continue; }
            item.bytes = std::make_shared<matter::VkBufferResource>(); item.size = size;
            if (!host_buffer(size, *item.bytes, error) ||
                !vulkan_.retain_for_frame(frame, {item.bytes->lifetime, item.blas->lifetime}, error)) { it = items.erase(it); continue; }
            VkCopyAccelerationStructureToMemoryInfoKHR copy{VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_TO_MEMORY_INFO_KHR};
            copy.src = item.blas->handle; copy.dst.deviceAddress = item.bytes->address + aligned_offset(*item.bytes);
            copy.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_SERIALIZE_KHR;
            serialize_(frame.command_buffer, &copy);
            VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT; barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.memoryBarrierCount = 1; dependency.pMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(frame.command_buffer, &dependency);
            item.query.reset(); item.serial = frame.serial; item.submitted = false; ++it;
        }
    }
private:
    struct Query {
        VkDevice device = VK_NULL_HANDLE; VkQueryPool pool = VK_NULL_HANDLE;
        ~Query() { if (pool) vkDestroyQueryPool(device, pool, nullptr); }
    };
    struct Capture {
        uint64_t serial; bool submitted; std::string directory, key;
        std::shared_ptr<matter::VkAccelerationStructureResource> blas;
        std::shared_ptr<Query> query;
        std::shared_ptr<matter::VkBufferResource> bytes;
        uint64_t size;
        // Keep the residency reservation charged while serialization retains
        // the BLAS, even if its page is evicted before readback completes.
        std::shared_ptr<const void> reservation;
        BlasDiskCache::Bytes ready_bytes;
    };
    matter::VulkanDevice& vulkan_;
    std::string fingerprint_;
    std::map<uint32_t, std::vector<Capture>> pending_;
    PFN_vkCmdCopyAccelerationStructureToMemoryKHR serialize_ = nullptr;
    PFN_vkCmdCopyMemoryToAccelerationStructureKHR deserialize_ = nullptr;
    PFN_vkGetDeviceAccelerationStructureCompatibilityKHR compatibility_ = nullptr;
    PFN_vkCmdWriteAccelerationStructuresPropertiesKHR query_ = nullptr;
    static VkDeviceSize aligned_offset(const matter::VkBufferResource& buffer) { return (256 - (buffer.address & 255)) & 255; }
    bool host_buffer(size_t size, matter::VkBufferResource& buffer, std::string& error) {
        return matter::create_buffer(vulkan_, size + 255,
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, buffer, error) && matter::map_buffer(buffer, error);
    }
};
} // namespace viewer
