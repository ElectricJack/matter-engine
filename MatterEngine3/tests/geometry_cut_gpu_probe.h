#pragma once
#include "geometry/geometry_hierarchy.h"
#include "render/lod_distance.h"
#include "render/vk_pipeline.h"
#include "render/vk_resources.h"
#include <array>
#include <map>

namespace geometry_cut_gpu_probe {
struct alignas(16) Node {
    float lo[4]{}, hi[4]{};
    uint32_t links[4]{UINT32_MAX, UINT32_MAX, 1, 0};
};
struct alignas(16) Params {
    float transform[16]{};
    float eye_reach[4]{};
    float scale_pad[4]{};
    uint32_t limits[4]{}, capacities[4]{};
    float frustum_planes[6][4]{};
};
static_assert(sizeof(Node) == 48 && sizeof(Params) == 224, "geometry cut shader ABI");
class Probe {
    matter::VulkanDevice& vk_;
    matter::VkComputePipelineResource pipeline_;
    matter::VkBufferResource buffers_[4];
    static void record(VkCommandBuffer command, void* opaque) {
        auto& probe = *static_cast<Probe*>(opaque);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &barrier, 0, nullptr, 0, nullptr);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, probe.pipeline_.pipeline);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, probe.pipeline_.pipeline_layout,
                                0, 1, &probe.pipeline_.descriptor_set, 0, nullptr);
        vkCmdDispatch(command, 1, 1, 1);
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 1, &barrier, 0, nullptr, 0, nullptr);
    }
public:
    explicit Probe(matter::VulkanDevice& vk) : vk_(vk) {}
    bool run(const Params& params, const std::vector<Node>& nodes, const std::vector<uint32_t>& roots,
             std::vector<uint32_t>& output, std::string& error) {
        if (!pipeline_.pipeline) {
            std::vector<VkDescriptorSetLayoutBinding> bindings(4);
            for (uint32_t i = 0; i < 4; ++i) {
                bindings[i].binding = i; bindings[i].descriptorCount = 1;
                bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            }
            if (!matter::create_compute_pipeline(vk_, "geometry_cut_probe.comp.spv", bindings, pipeline_, error)) return false;
        }
        output.resize(8 + params.limits[3] + params.capacities[0]);
        const VkDeviceSize sizes[] = {sizeof(params), nodes.size()*sizeof(Node), roots.size()*sizeof(uint32_t), output.size()*sizeof(uint32_t)};
        const void* inputs[] = {&params, nodes.data(), roots.data()};
        for (uint32_t i = 0; i < 4; ++i) {
            if (buffers_[i].size < sizes[i] && !matter::create_buffer(vk_, sizes[i], VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, buffers_[i], error)) return false;
            if (i < 3 && !matter::upload_buffer(vk_, buffers_[i], inputs[i], sizes[i], 0, error)) return false;
            matter::write_storage_buffer_descriptor(pipeline_, i, buffers_[i], 0, sizes[i]);
        }
        std::vector<std::shared_ptr<void>> keep{pipeline_.lifetime};
        for (auto& buffer : buffers_) keep.push_back(buffer.lifetime);
        return matter::submit_immediate(vk_, record, this, error, matter::ImmediateSubmitPhase::compute_dispatch, std::move(keep)) &&
            matter::readback_buffer(vk_, buffers_[3], output.data(), sizes[3], 0, error);
    }
};
inline void check(matter::VulkanDevice& vk, asset_store::PageCache& cache,
                  const std::vector<geometry::NodeRef>& roots) {
    std::string error;
    std::vector<geometry::NodeView> views;
    std::map<asset_store::BlobHash, uint32_t> ids;
    std::vector<geometry::NodeRef> todo = roots;
    for (size_t i = 0; i < todo.size(); ++i) {
        if (ids.count(todo[i].page)) continue;
        geometry::NodeView node;
        CHECK(geometry::decode_node(cache.read({todo[i].page})[0].page, node, error), error.c_str());
        if (!node.page) return;
        ids[node.self.page] = static_cast<uint32_t>(views.size());
        todo.insert(todo.end(), node.children.begin(), node.children.end()); views.push_back(std::move(node));
    }
    std::vector<Node> nodes(views.size()); std::vector<uint32_t> root_ids;
    for (const auto& root : roots) root_ids.push_back(ids.at(root.page));
    for (size_t i = 0; i < views.size(); ++i) {
        for (size_t k = 0; k < 3; ++k) { nodes[i].lo[k] = views[i].self.bounds.lo[k]; nodes[i].hi[k] = views[i].self.bounds.hi[k]; }
        nodes[i].lo[3] = float(views[i].self.error);
        if (double(nodes[i].lo[3]) < views[i].self.error) nodes[i].lo[3] = std::nextafter(nodes[i].lo[3], INFINITY);
        for (size_t k = 0; k < views[i].children.size(); ++k) nodes[i].links[k] = ids.at(views[i].children[k].page);
    }
    Probe probe(vk); uint32_t cases = 0;
    for (uint32_t scenario = 0; scenario < 24; ++scenario) {
        Params params;
        const float scale = scenario % 3 == 0 ? 3.f : 1.f;
        params.transform[0] = params.transform[5] = params.transform[10] = scale; params.transform[15] = 1;
        params.eye_reach[2] = scenario % 4 == 0 ? 1000.f : scenario % 4 == 1 ? 20.f : 0.f;
        params.eye_reach[3] = float(lod::error_switch_distance(1, scale, .002, 1)); params.scale_pad[0] = scale;
        geometry::CutConfig config;
        if (scenario >= 8 && scenario < 12) config.max_nodes = 1;
        if (scenario >= 12 && scenario < 16) config.max_selected = 2;
        if (scenario >= 16) config.max_requests = scenario % 2;
        params.limits[0] = uint32_t(nodes.size()); params.limits[1] = uint32_t(roots.size());
        params.limits[2] = config.max_nodes; params.limits[3] = config.max_selected; params.capacities[0] = config.max_requests;
        for (size_t i = 0; i < nodes.size(); ++i) nodes[i].links[2] = (scenario >= 16 && i > 0 && i % 3 == 1) ? 0 : 1;
        geometry::Cut expected;
        auto lookup = [&](const geometry::NodeRef& ref, geometry::NodeView& out) {
            const auto id = ids.at(ref.page); if (!nodes[id].links[2]) return false; out = views[id]; return true;
        };
        auto refine = [&](const geometry::NodeRef& ref) {
            const auto& n = nodes[ids.at(ref.page)]; float distance2 = 0, radius2 = 0;
            for (size_t k = 0; k < 3; ++k) {
                const float delta = (n.lo[k]+n.hi[k])*.5f*scale - params.eye_reach[k]; distance2 += delta*delta;
                const float extent = n.hi[k]-n.lo[k]; radius2 += extent*extent;
            }
            return n.lo[3] > 0 && std::max(.01f, std::sqrt(distance2) - .5f*std::sqrt(radius2)*scale) < n.lo[3]*params.eye_reach[3];
        };
        CHECK(geometry::select_cut(roots, lookup, refine, config, expected, error), error.c_str());
        std::vector<uint32_t> output;
        CHECK(probe.run(params, nodes, root_ids, output, error), error.c_str()); if (output.size() < 8) return;
        CHECK(output[4] == 0 && output[0] == expected.selected.size() && output[1] == expected.requests.size() &&
              output[2] == expected.visited && output[3] == expected.fallback_groups, "GPU hierarchy counters match independent CPU reference");
        for (size_t i = 0; i < expected.selected.size() && i < output[0]; ++i)
            CHECK(output[8+i] == ids.at(expected.selected[i].self.page), "GPU hierarchy selects the same complete cut");
        for (size_t i = 0; i < expected.requests.size() && i < output[1]; ++i)
            CHECK(output[8+config.max_selected+i] == ids.at(expected.requests[i]), "GPU missing-page feedback matches CPU reference");
        ++cases;
    }
    // Independent single-root fixture: a hidden root must not request its
    // missing children; boundary-touching and sheared boxes remain visible.
    std::vector<Node> fixture(3);
    for(auto& n:fixture){for(int k=0;k<3;++k){n.lo[k]=-1;n.hi[k]=1;}n.lo[3]=10;}
    fixture[0].links[0]=1;fixture[0].links[1]=2;
    fixture[1].links[2]=fixture[2].links[2]=0;
    Params visible;
    visible.transform[0]=visible.transform[5]=visible.transform[10]=visible.transform[15]=1;
    visible.eye_reach[3]=100;visible.scale_pad[0]=1;
    visible.limits[0]=3;visible.limits[1]=1;visible.limits[2]=10;visible.limits[3]=10;visible.capacities[0]=2;
    for(int scenario=0;scenario<4;++scenario){
        auto p=visible;
        p.frustum_planes[0][0]=1;
        p.frustum_planes[0][3]=scenario==0?-2.f:-1.f;
        if(scenario==2){p.transform[0]=-2;p.transform[4]=3;p.frustum_planes[0][3]=-5;}
        if(scenario==3){p.transform[12]=10;p.frustum_planes[0][3]=-10;}
        std::vector<uint32_t> output;
        CHECK(probe.run(p,fixture,{0},output,error),error.c_str());if(output.size()<8)return;
        CHECK(output[4]==0,"visibility keeps valid hierarchy admission");
        if(scenario==0)CHECK(output[0]==0 && output[1]==0,"off-screen geometry neither emits nor requests fine pages");
        else CHECK(output[0]==1 && output[1]==2,"boundary, reflection/shear and translation preserve visible fallback and refinement requests");
    }
    // All pages are resident, but a one-node traversal budget cannot reach
    // the requested leaves. No page request must not mean detail is ready.
    {
        std::vector<Node> limited(7, fixture[0]);
        for (auto& node : limited) {
            node.links[0]=node.links[1]=UINT32_MAX; node.links[2]=1;
        }
        limited[0].links[0]=1;limited[0].links[1]=2;
        limited[1].links[0]=3;limited[1].links[1]=4;
        limited[2].links[0]=5;limited[2].links[1]=6;
        auto params=visible; params.limits[0]=7;params.limits[2]=1;
        std::vector<uint32_t> output;
        CHECK(probe.run(params,limited,{0},output,error),error.c_str());
        if(output.size()>=8)
            CHECK(output[0]==2 && output[1]==0 && output[3]==2 && output[4]==0,
                  "GPU budget fallback remains unresolved without missing-page requests");
    }
    std::printf("Geometry GPU traversal: %u CPU/GPU comparisons, %zu unique nodes\n", cases, nodes.size());
}
} // namespace geometry_cut_gpu_probe
