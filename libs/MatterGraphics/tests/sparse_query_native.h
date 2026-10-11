// Fixture-only acceleration builder stands in for the independently owned RT
// producer. MatterGraphics consumes the completed product and never builds AS.
namespace {
struct QueryFixture {
  struct Buffer {
    VkDevice device;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceAddress address = 0;
    explicit Buffer(VkDevice d) : device(d) {}
    ~Buffer() {
      if (buffer)
        vkDestroyBuffer(device, buffer, nullptr);
      if (memory)
        vkFreeMemory(device, memory, nullptr);
    }
  };
  std::shared_ptr<NativeDevice> native;
  std::vector<std::shared_ptr<Buffer>> buffers;
  std::vector<VkAccelerationStructureKHR> structures;
  PFN_vkCreateAccelerationStructureKHR create_as;
  PFN_vkDestroyAccelerationStructureKHR destroy_as;
  PFN_vkGetAccelerationStructureBuildSizesKHR build_sizes;
  PFN_vkCmdBuildAccelerationStructuresKHR build_as;
  PFN_vkGetAccelerationStructureDeviceAddressKHR get_address;
  VkCommandPool pool = VK_NULL_HANDLE;
  uint32_t alignment = 256;
  explicit QueryFixture(std::shared_ptr<NativeDevice> n) : native(std::move(n)) {
    const auto d = native->device;
    create_as = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(
        vkGetDeviceProcAddr(d, "vkCreateAccelerationStructureKHR"));
    destroy_as = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(
        vkGetDeviceProcAddr(d, "vkDestroyAccelerationStructureKHR"));
    build_sizes = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(
        vkGetDeviceProcAddr(d, "vkGetAccelerationStructureBuildSizesKHR"));
    build_as = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(
        vkGetDeviceProcAddr(d, "vkCmdBuildAccelerationStructuresKHR"));
    get_address = reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(
        vkGetDeviceProcAddr(d, "vkGetAccelerationStructureDeviceAddressKHR"));
    check(create_as && destroy_as && build_sizes && build_as && get_address, "QUERY AS entrypoints");
    VkPhysicalDeviceAccelerationStructurePropertiesKHR as{};
    as.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
    VkPhysicalDeviceProperties2 p{};
    p.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    p.pNext = &as;
    vkGetPhysicalDeviceProperties2(native->physical, &p);
    alignment = as.minAccelerationStructureScratchOffsetAlignment;
    VkCommandPoolCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    ci.queueFamilyIndex = native->family;
    vkcheck(vkCreateCommandPool(d, &ci, nullptr, &pool), "QUERY command pool");
  }
  ~QueryFixture() {
    for (auto as : structures)
      destroy_as(native->device, as, nullptr);
    if (pool)
      vkDestroyCommandPool(native->device, pool, nullptr);
  }
  std::shared_ptr<Buffer> buffer(const void *bytes, VkDeviceSize size, VkBufferUsageFlags usage) {
    const auto d = native->device;
    auto out = std::make_shared<Buffer>(d);
    VkBufferCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    ci.size = size;
    ci.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    vkcheck(vkCreateBuffer(d, &ci, nullptr, &out->buffer), "QUERY buffer");
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(d, out->buffer, &req);
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(native->physical, &props);
    uint32_t type = UINT32_MAX;
    const VkMemoryPropertyFlags needed =
        bytes ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
              : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
      if ((req.memoryTypeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags & needed) == needed) {
        type = i;
        break;
      }
    check(type != UINT32_MAX, "QUERY memory type");
    VkMemoryAllocateFlagsInfo flags{};
    flags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.pNext = &flags;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    vkcheck(vkAllocateMemory(d, &ai, nullptr, &out->memory), "QUERY memory");
    vkcheck(vkBindBufferMemory(d, out->buffer, out->memory, 0), "QUERY bind");
    if (bytes) {
      void *mapped = nullptr;
      vkcheck(vkMapMemory(d, out->memory, 0, size, 0, &mapped), "QUERY map");
      std::memcpy(mapped, bytes, size_t(size));
      vkUnmapMemory(d, out->memory);
    }
    VkBufferDeviceAddressInfo address{};
    address.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    address.buffer = out->buffer;
    out->address = vkGetBufferDeviceAddress(d, &address);
    buffers.push_back(out);
    return out;
  }
  VkDeviceAddress address(VkAccelerationStructureKHR as) {
    VkAccelerationStructureDeviceAddressInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    info.accelerationStructure = as;
    return get_address(native->device, &info);
  }
  VkAccelerationStructureKHR build(const VkAccelerationStructureGeometryKHR &geometry, uint32_t count,
                                   bool top = false) {
    const auto d = native->device;
    VkAccelerationStructureBuildGeometryInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    info.type =
        top ? VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR : VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    info.geometryCount = 1;
    info.pGeometries = &geometry;
    VkAccelerationStructureBuildSizesInfoKHR sizes{};
    sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    build_sizes(d, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &count, &sizes);
    auto storage = buffer(nullptr, sizes.accelerationStructureSize,
                          VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR);
    VkAccelerationStructureCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    ci.buffer = storage->buffer;
    ci.size = sizes.accelerationStructureSize;
    ci.type = info.type;
    VkAccelerationStructureKHR as = VK_NULL_HANDLE;
    vkcheck(create_as(d, &ci, nullptr, &as), "QUERY create AS");
    structures.push_back(as);
    info.dstAccelerationStructure = as;
    auto scratch = buffer(nullptr, sizes.buildScratchSize + alignment, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    info.scratchData.deviceAddress = (scratch->address + alignment - 1) & ~VkDeviceAddress(alignment - 1);
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkcheck(vkAllocateCommandBuffers(d, &ai, &cmd), "QUERY command");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkcheck(vkBeginCommandBuffer(cmd, &begin), "QUERY begin");
    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = count;
    const auto *ranges = &range;
    build_as(cmd, 1, &info, &ranges);
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);
    vkcheck(vkEndCommandBuffer(cmd), "QUERY end");
    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    vkcheck(vkCreateFence(d, &fi, nullptr, &fence), "QUERY fence");
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    {
      std::lock_guard<std::mutex> lock(*native->mutex);
      vkcheck(vkQueueSubmit(native->queue, 1, &si, fence), "QUERY submit");
    }
    vkcheck(vkWaitForFences(d, 1, &fence, VK_TRUE, UINT64_MAX), "QUERY completed product");
    vkDestroyFence(d, fence, nullptr);
    vkFreeCommandBuffers(d, pool, 1, &cmd);
    return as;
  }
  VkAccelerationStructureKHR tlas(VkAccelerationStructureKHR bottom) {
    VkAccelerationStructureInstanceKHR instance{};
    for (int i = 0; i < 3; ++i)
      instance.transform.matrix[i][i] = 1;
    instance.mask = 0xff;
    instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    instance.accelerationStructureReference = address(bottom);
    auto data = buffer(&instance, sizeof(instance),
                       VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR);
    VkAccelerationStructureGeometryKHR geometry{};
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    geometry.geometry.instances.data.deviceAddress = data->address;
    return build(geometry, 1, true);
  }
  mg::SparseQueryProduct product(const mg::SparseSurfaceInputs &in,
                                 const std::shared_ptr<QueryFixture> &owner) {
    std::vector<mc::Vec3> vertices;
    for (const auto &t : in.surfaces)
      for (const auto &v : t.vertices)
        vertices.push_back(v.position);
    auto data = buffer(vertices.data(), vertices.size() * sizeof(mc::Vec3),
                       VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR);
    VkAccelerationStructureGeometryKHR geometry{};
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    auto &tri = geometry.geometry.triangles;
    tri.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    tri.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    tri.vertexData.deviceAddress = data->address;
    tri.vertexStride = sizeof(mc::Vec3);
    tri.maxVertex = uint32_t(vertices.size() - 1);
    tri.indexType = VK_INDEX_TYPE_NONE_KHR;
    const auto inner = tlas(build(geometry, uint32_t(in.surfaces.size())));
    VkAabbPositionsKHR bounds{-2, -1, -2.01f, 2, 1, -1.99f};
    data =
        buffer(&bounds, sizeof(bounds), VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR);
    geometry = {};
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_AABBS_KHR;
    auto &aabb = geometry.geometry.aabbs;
    aabb.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR;
    aabb.data.deviceAddress = data->address;
    aabb.stride = sizeof(bounds);
    mg::SparseQueryProduct product;
    product.device = native->device;
    product.forest = tlas(build(geometry, 1));
    product.lifetime = owner;
    product.objects = {{address(inner), 0, 1}};
    product.local_instances = in.placements;
    mg::SparseQueryPlacement root;
    root.token = 9;
    product.placements = {root};
    return product;
  }
};
void exercise_sparse_query(const std::shared_ptr<NativeDevice> &native) {
  mg::VulkanSparseSurface sparse(native->binding(native), {}, {true});
  mg::SparseSurfaceOutput out, reference;
  auto in = sparse_plane();
  valid(sparse.render(in, reference), "QUERY proxy reference");
  auto owner = std::make_shared<QueryFixture>(native);
  std::weak_ptr<QueryFixture> lifetime = owner;
  in.query = owner->product(in, owner);
  owner.reset();
  in.placements.clear();
  valid(sparse.render(in, out), "QUERY nested traversal");
  equal_gbuffer(out.gbuffer, reference.gbuffer, "QUERY raster/traversal parity");
  check(!lifetime.expired(), "QUERY completed product retained");
  mg::SurfaceTextureMip mip;
  mip.texels = {{{0x00996633u, 0xffff8080u}}};
  in.textures = {{{mip}}};
  for (auto &t : in.surfaces)
    t.texture = 0;
  valid(sparse.render(in, out), "QUERY alpha reject");
  for (const auto &p : out.gbuffer.identity.pixels)
    check(!p[1], "QUERY ray alpha cutout");
  in.textures[0].mips[0].texels[0][0] |= 0xff000000u;
  valid(sparse.render(in, out), "QUERY texture update");
  for (const auto &p : out.gbuffer.albedo.pixels)
    near_value(p[0], .4 * .2, "QUERY filtered tint");
  const auto before = out.gbuffer.identity.pixels;
  in.query->device = VK_NULL_HANDLE;
  check(!sparse.render(in, out).ok() && out.gbuffer.identity.pixels == before,
        "QUERY foreign owner atomic rejection");
  in.query->device = native->device;
  mg::VulkanSparseSurface disabled(native->binding(native));
  const auto rejected = disabled.render(in, out);
  check(!rejected.ok() && rejected.diagnostics[0].code == mc::ErrorCode::unsupported_capability,
        "QUERY device feature contract");
  in.query.reset();
  check(lifetime.expired(), "QUERY product release");
  std::cout << "PASS SPARSE-QUERY/EXTERNAL-AS/RASTER-PARITY/ALPHA/TEXTURE/FEATURE/OWNERSHIP\n";
}
} // namespace
