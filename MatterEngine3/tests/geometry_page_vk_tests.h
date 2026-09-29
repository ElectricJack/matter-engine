#pragma once
#include "geometry/geometry_residency.h"
#include "render/geometry_raster_adapter.h"
#include "render/matrix_math.h"
#include "geometry_cut_gpu_probe.h"
#include <filesystem>

namespace geometry_page_vk_test {
inline MeshIndexed surface(uint32_t n) {
    MeshIndexed mesh;
    for (uint32_t y = 0; y <= n; ++y) for (uint32_t x = 0; x <= n; ++x) {
        const float u = float(x) / n, v = float(y) / n;
        mesh.positions.push_back(make_float3(2*u-1, 2*v-1, -3 + .25f*std::sin(6*u)*std::sin(6*v)));
    }
    for (uint32_t y = 0; y < n; ++y) for (uint32_t x = 0; x < n; ++x) {
        uint32_t a = y*(n+1)+x, b = a+1, c = a+n+1, d = c+1;
        for (auto triangle : {std::array<uint32_t,3>{a,b,d}, std::array<uint32_t,3>{a,d,c}}) {
            TriEx extra{}; extra.materialId = 7; extra.tint = make_float4(1,1,1,0);
            extra.N0 = extra.N1 = extra.N2 = make_float3(0,0,1);
            extra.ao0 = extra.ao1 = extra.ao2 = 1;
            float2* uv[] = {&extra.uv0, &extra.uv1, &extra.uv2};
            for (size_t k = 0; k < 3; ++k) {
                mesh.indices.push_back(triangle[k]); const auto p = mesh.positions[triangle[k]];
                *uv[k] = {(p.x+1)*.5f, (p.y+1)*.5f};
            }
            mesh.triex.push_back(extra);
        }
    }
    return mesh;
}
inline void run(matter::VulkanDevice& vulkan) {
    CHECK(vulkan.ray_tracing_available(), "geometry pages require native RT for this acceptance test");
    if (!vulkan.ray_tracing_available()) return;
    const auto directory = std::filesystem::temp_directory_path() /
        ("matter-geometry-vk-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    {
        std::string error;
        asset_store::StoreConfig config; config.dir = directory.string();
        auto store = asset_store::BlobStore::open(config, &error);
        CHECK(store != nullptr, error.c_str()); if (!store) return;
        auto refs = asset_store::RefTable::open(*store, {}, &error);
        CHECK(refs != nullptr, error.c_str()); if (!refs) return;
        geometry::Hierarchy hierarchy;
        geometry::CompileConfig compiler; compiler.leaf_triangles = 32;
        compiler.verification.tolerance = .01; compiler.verification.max_depth = 8;
        const auto source = surface(16);
        CHECK(geometry::compile(source, compiler, hierarchy, error), error.c_str());
        std::vector<geometry::NodeRef> roots;
        CHECK(geometry::write_hierarchy(hierarchy, *store, *refs, "surface", {}, roots, error), error.c_str());
        if (roots.empty()) return;
        geometry::Node reference; reference.mesh = source; reference.source_triangles = static_cast<uint32_t>(source.triex.size());
        reference.bounds = hierarchy.nodes[hierarchy.roots[0]].bounds;
        geometry::Hierarchy full; full.nodes.push_back(reference); full.roots.push_back(0);
        std::vector<geometry::NodeRef> full_roots;
        CHECK(geometry::write_hierarchy(full, *store, *refs, "reference", {}, full_roots, error), error.c_str());
        asset_store::PageCacheConfig cache_config; cache_config.store = config;
        auto cache = asset_store::PageCache::open(cache_config, error);
        CHECK(cache != nullptr, error.c_str()); if (!cache || full_roots.empty()) return;
        {
            auto probe_cache = asset_store::PageCache::open(cache_config, error);
            CHECK(probe_cache != nullptr, error.c_str());
            if (probe_cache) geometry_cut_gpu_probe::check(vulkan, *probe_cache, roots);
        }
        geometry::NodeView reference_page;
        CHECK(geometry::decode_node(cache->read({full_roots[0].page})[0].page, reference_page, error), error.c_str());
        viewer::VkSceneRenderer renderer(vulkan);
        CHECK(renderer.init(error), error.c_str()); if (!error.empty()) return;
        std::vector<MaterialGpuRecord> materials(8);
        materials[7].base_roughness[0] = .5f; materials[7].base_roughness[1] = .25f;
        materials[7].base_roughness[2] = .12f; materials[7].base_roughness[3] = .8f;
        materials[7].metal_opacity_spec_coat[1] = 1; materials[7].scattering_shape[3] = 1;
        CHECK(renderer.update_materials(materials, 1, 1, error), error.c_str());
        viewer::VkScenePart reference_part;
        CHECK(viewer::build_geometry_page_part(100, reference_page, reference_part, error), error.c_str());
        // Runtime passes only the immutable page handle. The adapter must
        // validate bytes and derive bounds itself, without trusting NodeView metadata.
        geometry::NodeView handle_only; handle_only.page = reference_page.page;
        viewer::VkScenePart direct_part;
        CHECK(viewer::build_geometry_page_part(100, handle_only, direct_part, error), error.c_str());
        CHECK(direct_part.indices == reference_part.indices &&
              direct_part.vertices.size() == reference_part.vertices.size() &&
              std::memcmp(direct_part.vertices.data(), reference_part.vertices.data(),
                          reference_part.vertices.size() * sizeof(viewer::VkRasterVertex)) == 0 &&
              direct_part.clusters[0].aabb_min.x == reference_part.clusters[0].aabb_min.x &&
              direct_part.clusters[0].radius == reference_part.clusters[0].radius,
              "page-handle adapter derives identical vertices and bounds from validated bytes");
        CHECK(reference_part.vertices.size() < reference_part.indices.size(), "geometry adapter preserves indexed sharing");
        CHECK(reference_part.indices.size() == source.indices.size(), "geometry adapter preserves corner count");
        for (size_t i = 0; i < reference_part.indices.size(); ++i) {
            const auto& vertex = reference_part.vertices[reference_part.indices[i]];
            const auto position = source.positions[source.indices[i]];
            const auto& ex = source.triex[i / 3];
            const float2 uv[] = {ex.uv0, ex.uv1, ex.uv2};
            CHECK(vertex.position.x == position.x && vertex.position.y == position.y && vertex.position.z == position.z &&
                  vertex.surface.x == uv[i%3].x && vertex.surface.y == uv[i%3].y &&
                  vertex.material_index == 7 && vertex.normal.z == 1 && vertex.tint.x == 1,
                  "geometry adapter preserves per-corner positions, normals, UVs, material and tint");
        }
        CHECK(renderer.ensure_part(reference_part, error) >= 0, error.c_str());
        CHECK(renderer.update_instances({{100, viewer::mat4_identity(), 100}}, error), error.c_str());
        matter::VulkanRayTracingSettings rt{}; rt.enabled = true; rt.max_distance = 100;
        renderer.set_ray_tracing_settings(rt);
        matter::VulkanGiSettings gi{}; gi.enabled = 1; gi.samples_per_pixel = 1; gi.max_bounces = 1;
        renderer.set_gi_settings(gi);
        matter::CameraDesc camera{}; camera.position = {0,0,0}; camera.target = {0,0,-1};
        camera.up = {0,1,0}; camera.vertical_fov_radians = 1.57079632679f;
        camera.near_plane = .1f; camera.far_plane = 20;
        auto frame = [&]() {
            matter::VulkanFrame frame{}; viewer::FrameMatrices matrices{};
            bool ok = vulkan.begin_frame(frame, error) &&
                viewer::build_frame_matrices(camera, frame.extent.width, frame.extent.height, matrices, error) &&
                renderer.prepare_frame(frame, matrices, camera.position, 1, error) &&
                renderer.record_cull_and_render(frame, matrices, camera.position, 1, error) &&
                renderer.record_composite_to_swapchain(frame, error) && vulkan.end_frame(frame, error);
            renderer.finish_ray_tracing_frame(frame.serial, ok);
            renderer.finish_dynamic_frame(frame.serial); vulkan.wait_idle();
            CHECK(ok, error.c_str()); return ok;
        };
        if (!frame()) return;
        viewer::VkRasterPixel reference_pixel;
        auto extent = renderer.test_opaque_extent();
        CHECK(renderer.readback_raster_pixel(extent.width/2, extent.height/2, reference_pixel, error), error.c_str());
        CHECK(reference_pixel.material_index == 7 && reference_pixel.depth < 1, "full source reference rasterized with source material");
        // Persistent, device-compatible BLAS round trip. Runtime page IDs may
        // change; cache identity must follow immutable geometry content.
        auto cached_part = reference_part; cached_part.part_hash = 900;
        cached_part.blas_cache_directory = (directory / "blas").string();
        cached_part.blas_cache_content = asset_store::hash_to_string(full_roots[0].page);
        cached_part.geometry_budget_claim = std::make_shared<const int>(1);
        std::weak_ptr<const void> capture_claim = cached_part.geometry_budget_claim;
        CHECK(renderer.ensure_part(cached_part, error) >= 0 && renderer.queue_geometry_page_warmup(900, error), error.c_str());
        for (int i=0; i<60 && !renderer.geometry_page_render_ready(900); ++i) if (!frame()) return;
        CHECK(renderer.geometry_page_render_ready(900), "cold BLAS cache miss builds a usable page");
        renderer.release_part(900); cached_part.geometry_budget_claim.reset();
        CHECK(!capture_claim.expired(), "pending serialization keeps evicted BLAS charged to residency");
        for (int i=0; i<30 && renderer.blas_cache_stats()[2] == 0; ++i) if (!frame()) return;
        CHECK(renderer.blas_cache_stats()[2] == 1, "retired GPU serialization queued for durable storage");
        CHECK(capture_claim.expired(), "serialization retires its reservation after readback");
        cached_part.part_hash = 901;
        CHECK(renderer.ensure_part(cached_part, error) >= 0 && renderer.queue_geometry_page_warmup(901, error), error.c_str());
        for (int i=0; i<60 && !renderer.geometry_page_render_ready(901); ++i) if (!frame()) return;
        CHECK(renderer.geometry_page_render_ready(901) && renderer.blas_cache_stats()[0] == 1,
              "second page load deserializes its cached BLAS instead of rebuilding");
        CHECK(renderer.test_last_rt_blas_build_count() == 0, "cache hit records no triangle BLAS builds");
        CHECK(renderer.update_instances({{901, viewer::mat4_identity(), 100}}, error), error.c_str());
        if (!frame()) return;
        viewer::VkRasterPixel cached_pixel;
        CHECK(renderer.readback_raster_pixel(extent.width/2, extent.height/2, cached_pixel, error), error.c_str());
        CHECK(cached_pixel.material_index == reference_pixel.material_index && std::abs(cached_pixel.depth-reference_pixel.depth)<1e-6f,
              "cached BLAS page preserves source raster coverage");
        CHECK(renderer.test_last_rt_geometry_records().size() == 1 && renderer.test_last_rt_geometry_records()[0].part_hash == 901,
              "deserialized BLAS participates in RT scene");
        CHECK(renderer.update_instances({{100, viewer::mat4_identity(), 100}}, error), error.c_str());
        renderer.release_part(901);
        // More pages than the disk lookup queue can admit at once. Completed
        // payloads must survive deferred GPU submission without becoming misses.
        const auto hits_before_batch = renderer.blas_cache_stats()[0];
        const auto misses_before_batch = renderer.blas_cache_stats()[1];
        for (uint64_t id = 910; id < 958; ++id) {
            cached_part.part_hash = id;
            CHECK(renderer.ensure_part(cached_part, error) >= 0 &&
                  renderer.queue_geometry_page_warmup(id, error), error.c_str());
        }
        bool batch_ready = false;
        for (int i = 0; i < 120 && !batch_ready; ++i) {
            if (!frame()) return;
            CHECK(renderer.test_last_rt_blas_build_count() == 0,
                  "queued cache hits never fall back to triangle builds");
            batch_ready = true;
            for (uint64_t id = 910; id < 958; ++id)
                batch_ready &= renderer.geometry_page_render_ready(id);
        }
        CHECK(batch_ready && renderer.blas_cache_stats()[0] == hits_before_batch + 48,
              "cache restores drain a batch larger than disk queue capacity");
        CHECK(renderer.blas_cache_stats()[1] == misses_before_batch,
              "lookup backpressure and deferred restores do not become cache misses");
        for (uint64_t id = 910; id < 958; ++id) renderer.release_part(id);
        // A burst larger than the old 32-capture limit must all persist.
        const auto captures_before_batch = renderer.blas_cache_stats()[2];
        for (uint64_t id = 970; id < 1018; ++id) {
            cached_part.part_hash = id;
            cached_part.blas_cache_content = "capture-burst/" + std::to_string(id);
            CHECK(renderer.ensure_part(cached_part, error) >= 0 &&
                  renderer.queue_geometry_page_warmup(id, error), error.c_str());
        }
        for (int i = 0; i < 180 && renderer.blas_cache_stats()[2] < captures_before_batch + 48; ++i)
            if (!frame()) return;
        CHECK(renderer.blas_cache_stats()[2] == captures_before_batch + 48,
              "cold page burst retains every BLAS capture through serialization");
        for (uint64_t id = 970; id < 1018; ++id) renderer.release_part(id);
        const auto burst_hits = renderer.blas_cache_stats()[0];
        const auto burst_misses = renderer.blas_cache_stats()[1];
        for (uint64_t id = 970; id < 1018; ++id) {
            cached_part.part_hash = id;
            cached_part.blas_cache_content = "capture-burst/" + std::to_string(id);
            CHECK(renderer.ensure_part(cached_part, error) >= 0 &&
                  renderer.queue_geometry_page_warmup(id, error), error.c_str());
        }
        for (int i = 0; i < 180 && renderer.blas_cache_stats()[0] < burst_hits + 48; ++i)
            if (!frame()) return;
        CHECK(renderer.blas_cache_stats()[0] == burst_hits + 48 &&
              renderer.blas_cache_stats()[1] == burst_misses,
              "every captured burst page restores without a new cache miss");
        for (uint64_t id = 970; id < 1018; ++id) renderer.release_part(id);
        const auto cache_stats = renderer.blas_cache_stats();
        std::printf("BLAS cache: restored=%llu miss=%llu captured=%llu rejected=%llu\n",
            (unsigned long long)cache_stats[0], (unsigned long long)cache_stats[1],
            (unsigned long long)cache_stats[2], (unsigned long long)cache_stats[3]);
        geometry::Residency residency;
        auto lease = residency.attach(cache->read_manifest("surface").page, error);
        auto tickets = residency.dispatch(1, 0);
        CHECK(lease.id && tickets.size() == 1, "real disk-backed root admitted"); if (tickets.empty()) return;
        auto ticket = tickets.front();
        CHECK(residency.complete_read(ticket, cache->read({ticket.page})[0].page, error), error.c_str());
        geometry::NodeView root_page;
        CHECK(residency.staged_node(ticket, root_page), "decoded page available for upload");
        viewer::VkScenePart root_part;
        CHECK(viewer::build_geometry_page_part(200, root_page, root_part, error), error.c_str());
        uint64_t root_gpu_bytes = 0, root_scratch_bytes = 0;
        CHECK(renderer.geometry_page_upload_cost(root_part, root_gpu_bytes, root_scratch_bytes, error), error.c_str());
        uint64_t quoted_again=0,scratch_again=0;
        CHECK(renderer.geometry_page_upload_cost(root_part,quoted_again,scratch_again,error) &&
              quoted_again==root_gpu_bytes && scratch_again==root_scratch_bytes,
              "cached geometry upload quote preserves exact device sizing");
        auto invalid_quote=root_part;invalid_quote.indices.pop_back();
        CHECK(!renderer.geometry_page_upload_cost(invalid_quote,quoted_again,scratch_again,error),
              "quote cache does not bypass invalid triangle-count rejection");
        error.clear();
        CHECK(root_gpu_bytes > 0 && root_scratch_bytes > 0, "native Vulkan allocation requirements quoted");
        CHECK(residency.reserve_upload(ticket, root_gpu_bytes, root_scratch_bytes), "reserve before GPU work");
        auto upload_claim = residency.upload_reservation(ticket);
        CHECK(renderer.ensure_part(root_part, error) >= 0 && renderer.queue_geometry_page_warmup(200, error), error.c_str());
        CHECK(!renderer.geometry_page_render_ready(200), "unbuilt BLAS cannot publish geometry page");
        // The current full source remains the only visible/traced surface while
        // its coarse replacement builds off-screen through the same renderer.
        if (!frame()) return;
        CHECK(renderer.geometry_page_render_ready(200), "submitted page has raster and RT resources");
        const auto records = renderer.test_last_rt_geometry_records();
        CHECK(records.size() == 1 && records[0].part_hash == 100, "warm page never enters current TLAS membership");
        CHECK(residency.publish(ticket, renderer.geometry_page_resources(200)), "publish after actual raster/RT readiness");
        CHECK(renderer.update_instances({{200, viewer::mat4_identity(), 100}}, error), error.c_str());
        if (!frame()) return;
        viewer::VkRasterPixel root_pixel;
        CHECK(renderer.readback_raster_pixel(extent.width/2, extent.height/2, root_pixel, error), error.c_str());
        CHECK(root_pixel.material_index == reference_pixel.material_index && root_pixel.depth < 1,
              "coarse streamed root retains coverage and material");
        CHECK(renderer.test_last_rt_geometry_records().size() == 1 &&
              renderer.test_last_rt_geometry_records()[0].part_hash == 200,
              "raster and ray tracing publish the same root cut");
        CHECK(renderer.test_last_rt_blas_build_count() == 0, "published warm page reuses its prepared BLAS");
        std::printf("Geometry native root: source=%zu coarse=%u depth-reference=%.7f depth-root=%.7f\n",
                    source.triex.size(), root_page.self.triangles, reference_pixel.depth, root_pixel.depth);
        std::map<asset_store::BlobHash, uint64_t> gpu_ids{{root_page.self.page, 200}};
        uint64_t next_gpu_id = 201;
        const auto refine = [&](const geometry::NodeRef& node) {
            return node.page == root_page.self.page ||
                (!root_page.children.empty() && node.page == root_page.children[0].page);
        };
        geometry::ResidentCut cut;
        CHECK(residency.select(lease, refine, {}, cut, error), error.c_str());
        for (uint32_t arrival = 0; arrival < 8 && !cut.cut.requests.empty(); ++arrival) {
            // Delay siblings independently: an uploaded first child must not
            // replace its parent before the entire sibling set is ready.
            auto work = residency.dispatch(1, arrival);
            CHECK(work.size() == 1, "one independently demanded fine page dispatched");
            if (work.empty()) break;
            const auto request = work.front();
            CHECK(residency.complete_read(request, cache->read({request.page})[0].page, error), error.c_str());
            geometry::NodeView decoded;
            CHECK(residency.staged_node(request, decoded), "fine page decoded from binary cache");
            const uint64_t gpu_id = next_gpu_id++;
            viewer::VkScenePart part;
            CHECK(viewer::build_geometry_page_part(gpu_id, decoded, part, error), error.c_str());
            uint64_t gpu_bytes = 0, scratch_bytes = 0;
            CHECK(renderer.geometry_page_upload_cost(part, gpu_bytes, scratch_bytes, error), error.c_str());
            CHECK(residency.reserve_upload(request, gpu_bytes, scratch_bytes), "reserve fine GPU publication");
            auto retained_upload = residency.upload_reservation(request);
            CHECK(renderer.ensure_part(part, error) >= 0 && renderer.queue_geometry_page_warmup(gpu_id, error), error.c_str());
            // Geometry admission also progresses when the user disables RT
            // lighting. The page still prepares a BLAS for subsequent toggles.
            if (arrival == 0) { rt.enabled = false; renderer.set_ray_tracing_settings(rt); }
            if (!frame()) return;
            CHECK(renderer.geometry_page_render_ready(gpu_id), "fine page raster and BLAS are ready");
            CHECK(residency.publish(request, renderer.geometry_page_resources(gpu_id)), "publish actual fine GPU resources");
            gpu_ids[request.page] = gpu_id;
            if (arrival == 0) { rt.enabled = true; renderer.set_ray_tracing_settings(rt); }
            CHECK(residency.select(lease, refine, {}, cut, error), error.c_str());
            if (arrival == 0) CHECK(cut.cut.selected.size() == 1, "partial native child arrival retains parent");
            uint64_t covered = 0;
            std::vector<viewer::VkSceneInstance> instances;
            for (const auto& node : cut.cut.selected) {
                covered += node.self.source_triangles;
                instances.push_back({gpu_ids.at(node.self.page), viewer::mat4_identity(), gpu_ids.at(node.self.page)});
            }
            CHECK(covered == source.triex.size(), "native mixed cut covers original mesh exactly once");
            CHECK(renderer.update_instances(instances, error), error.c_str());
            if (!frame()) return;
            const auto traced = renderer.test_last_rt_geometry_records();
            CHECK(traced.size() == cut.cut.selected.size(), "one RT group per selected raster group");
            for (const auto& record : traced) {
                CHECK(std::any_of(instances.begin(), instances.end(), [&](const auto& instance) {
                    return record.part_hash == instance.part_hash;
                }), "TLAS contains only the published cut");
            }
            CHECK(renderer.test_last_rt_blas_build_count() == 0, "camera publication reuses prepared fine BLASes");
            for (int offset : {-18, 0, 18}) {
                viewer::VkRasterPixel pixel;
                CHECK(renderer.readback_raster_pixel(extent.width/2 + offset, extent.height/2, pixel, error), error.c_str());
                CHECK(pixel.material_index == 7 && pixel.depth < 1, "partial arrival preserves sampled raster coverage");
            }
        }
        CHECK(cut.cut.requests.empty() && cut.cut.selected.size() == 3,
              "one unique mesh renders simultaneous coarse and fine groups");
        std::printf("Geometry native mixed cut: groups=%zu cache-reads=%llu retained-GPU-bytes=%llu\n",
                    cut.cut.selected.size(), static_cast<unsigned long long>(cache->stats().disk_reads),
                    static_cast<unsigned long long>(residency.stats().gpu_bytes));
        geometry::ResidentHierarchy snapshot;
        CHECK(residency.snapshot(lease, snapshot, error), error.c_str());
        std::map<asset_store::BlobHash, uint32_t> node_ids;
        for (size_t i = 0; i < snapshot.nodes.size(); ++i) node_ids[snapshot.nodes[i].self.page] = static_cast<uint32_t>(i);
        std::vector<viewer::VkGeometryCutNode> gpu_nodes;
        std::vector<uint32_t> gpu_roots;
        for (const auto& root : snapshot.roots) gpu_roots.push_back(node_ids.at(root.page));
        for (const auto& node : snapshot.nodes) {
            viewer::VkGeometryCutNode gpu;
            gpu.page_lo = node.self.page.lo; gpu.page_hi = node.self.page.hi;
            std::copy(node.self.bounds.lo, node.self.bounds.lo+3, gpu.lo);
            std::copy(node.self.bounds.hi, node.self.bounds.hi+3, gpu.hi); gpu.error = static_cast<float>(node.self.error);
            for (size_t c = 0; c < node.children.size(); ++c) gpu.children[c] = node_ids.at(node.children[c].page);
            if (node.ready) gpu.ready_part_hash = gpu_ids.at(node.self.page);
            gpu_nodes.push_back(gpu);
        }
        for (bool fine : {true, false}) {
            geometry::ResidentCut expected;
            CHECK(residency.select(lease, [fine](const geometry::NodeRef&) { return fine; }, {}, expected, error), error.c_str());
            viewer::VkSceneInstance controller{100, viewer::mat4_identity(), 100}; controller.ray_traced = false;
            std::vector<viewer::VkSceneInstance> candidates{controller};
            for (const auto& node : snapshot.resources) {
                viewer::VkSceneInstance proxy{gpu_ids.at(node->node.self.page), viewer::mat4_identity(), 100};
                proxy.rt_proxy_only = true;
                proxy.ray_traced = std::any_of(expected.resources.begin(), expected.resources.end(), [&](const auto& selected) {
                    return selected->node.self.page == node->node.self.page;
                });
                candidates.push_back(proxy);
            }
            CHECK(renderer.set_geometry_cut(gpu_nodes, gpu_roots,
                {{0, 0, static_cast<uint32_t>(gpu_roots.size()), 1, fine ? 100000.f : 0.f, lease.id}}, error), error.c_str());
            CHECK(renderer.update_instances(candidates, error), error.c_str());
            if (!frame()) return;
            // Recycle frame slots: feedback is read only after their fences
            // retire. It must retain the owner/page identities of that frame.
            renderer.take_geometry_page_requests();
            for (int recycle = 0; recycle < 4; ++recycle) if (!frame()) return;
            const auto warmed_uploads = renderer.test_geometry_cut_uploads();
            for (int stable = 0; stable < 4; ++stable) if (!frame()) return;
            CHECK(renderer.test_geometry_cut_uploads() == warmed_uploads,
                  "unchanged hierarchy reuses every retired frame-slot upload");
            const auto feedback = renderer.take_geometry_page_requests();
            if (fine) {
                CHECK(!feedback.empty(), "GPU missing-page feedback survives asynchronous frame retirement");
                for (const auto& request : feedback) {
                    const asset_store::BlobHash page{request.page_lo, request.page_hi};
                    CHECK(request.owner_lease == lease.id &&
                        std::find(expected.cut.requests.begin(), expected.cut.requests.end(), page) != expected.cut.requests.end(),
                        "GPU request maps to the correct asset lease and missing child page");
                }
            }
            viewer::VkCullStats stats;
            CHECK(renderer.cull_stats(stats, error), error.c_str());
            uint32_t triangles = 0; for (const auto& node : expected.resources) triangles += node->node.self.triangles;
            CHECK(stats.emitted == expected.resources.size() && stats.triangles == triangles,
                  "GPU hierarchy writes exactly the selected indexed indirect draws");
            CHECK(renderer.test_last_rt_geometry_records().size() == expected.resources.size(),
                  "CPU RT reference matches GPU-authored geometry membership");
            viewer::VkRasterPixel pixel;
            CHECK(renderer.readback_raster_pixel(extent.width/2, extent.height/2, pixel, error), error.c_str());
            CHECK(pixel.material_index == 7 && pixel.depth < 1, "GPU hierarchy indirect draws preserve actual raster coverage");
        }
        // Raster hierarchy draws reserve buckets without one dummy instance
        // per resident page. Exercise both cuts with the same controller list.
        rt.enabled = false; renderer.set_ray_tracing_settings(rt);
        gi.enabled = 0; renderer.set_gi_settings(gi);
        // Put the immutable hierarchy after an unrelated node. Its children
        // remain block-local while roots and feedback identifiers are global.
        gpu_nodes.insert(gpu_nodes.begin(),viewer::VkGeometryCutNode{});
        for(auto& root:gpu_roots) ++root;
        for (bool fine : {true, false}) {
            for (auto& node : gpu_nodes) {
                if (node.ready_part_hash) {
                    CHECK(renderer.resolve_geometry_page(node), "capture ready geometry binding before worker packing");
                    // An obsolete or mismatched slot hint must resolve by the
                    // immutable page identity, never draw another page's range.
                    if (!fine) node.ready_slot ^= 1u;
                } else {
                    // Packed readiness words are not authority for an absent page.
                    node.ready=1;node.cluster=UINT32_MAX;
                }
            }
            geometry::ResidentCut expected;
            CHECK(residency.select(lease, [fine](const geometry::NodeRef&) { return fine; }, {}, expected, error), error.c_str());
            viewer::VkSceneInstance controller{100, viewer::mat4_identity(), 100};
            controller.ray_traced = false;
            CHECK(renderer.set_geometry_cut(gpu_nodes, gpu_roots,
                {{0, 0, static_cast<uint32_t>(gpu_roots.size()), 1, fine ? 100000.f : 0.f, lease.id, false, 1}}, error, true), error.c_str());
            CHECK(renderer.update_instances({controller}, error), error.c_str());
            const auto old_uploads = renderer.test_geometry_cut_uploads();
            if (!frame()) return;
            CHECK(renderer.test_geometry_cut_uploads() > old_uploads,
                  "changed hierarchy uploads before its next GPU dispatch");
            viewer::VkCullStats stats;
            CHECK(renderer.cull_stats(stats, error), error.c_str());
            uint32_t triangles = 0;
            for (const auto& node : expected.resources) triangles += node->node.self.triangles;
            CHECK(stats.emitted == expected.resources.size() && stats.triangles == triangles,
                  "controller-only hierarchy reserves complete page draw capacity");
            viewer::VkRasterPixel pixel;
            CHECK(renderer.readback_raster_pixel(extent.width/2, extent.height/2, pixel, error), error.c_str());
            CHECK(pixel.material_index == 7 && pixel.depth < 1,
                  "controller-only hierarchy preserves raster material and coverage");
        }
        CHECK(renderer.set_geometry_cut({}, {}, {}, error), error.c_str());
        // Raster publication must not allocate RT inputs or wait for a BLAS.
        auto raster_part = reference_part; raster_part.part_hash = 990;
        raster_part.geometry_raster_only = true;
        uint64_t raster_bytes = 0, raster_scratch = 1;
        CHECK(renderer.geometry_page_upload_cost(raster_part, raster_bytes, raster_scratch, error), error.c_str());
        CHECK(raster_scratch == 0 && raster_bytes == raster_part.vertices.size()*sizeof(viewer::VkRasterVertex) +
              raster_part.indices.size()*sizeof(uint32_t), "raster page charges only shared arena geometry");
        CHECK(renderer.ensure_part(raster_part, error) >= 0 && renderer.queue_geometry_page_warmup(990, error), error.c_str());
        CHECK(renderer.test_rt_geometry_address(990) == 0, "raster page allocates no RT vertex buffer");
        rt.enabled = false; renderer.set_ray_tracing_settings(rt);
        gi.enabled = 0; renderer.set_gi_settings(gi);
        CHECK(renderer.update_instances({{990, viewer::mat4_identity(), 990}}, error), error.c_str());
        if (!frame()) return;
        CHECK(renderer.geometry_page_render_ready(990) && renderer.geometry_page_resources(990),
              "raster-only page publishes without BLAS readiness");
        viewer::VkRasterPixel raster_pixel;
        CHECK(renderer.readback_raster_pixel(extent.width/2, extent.height/2, raster_pixel, error), error.c_str());
        CHECK(raster_pixel.material_index == reference_pixel.material_index &&
              std::abs(raster_pixel.depth-reference_pixel.depth)<1e-6f,
              "raster-only page preserves geometry coverage and material");
        CHECK(vulkan.validation_error_count() == 0, "geometry page raster/RT validation clean");
    }
    std::error_code ignored; std::filesystem::remove_all(directory, ignored);
}
} // namespace geometry_page_vk_test
