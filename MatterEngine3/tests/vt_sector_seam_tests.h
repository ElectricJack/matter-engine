#pragma once

// Opt-in acceptance gate for POM across independent world-surface owners.
// The reference has two connected charts in one owner. Split cases preserve
// the physical surface and world recipe but use translated local frames and
// independent VT slots. Unequal tessellation adds a nonmatching border mesh.
namespace vt_sector_seam_tests {

inline viewer::VkScenePart half(const viewer::VkScenePart& reference,
                                uint64_t hash, float center_x, int divisions) {
    auto part = reference;
    part.part_hash = hash;
    part.vertices.clear(); part.indices.clear();
    auto& mesh = part.lod_chart_meshes[0];
    mesh = {};
    mesh.dominant_material = 5;
    for (int y = 0; y <= divisions; ++y) for (int x = 0; x <= divisions; ++x) {
        const float u = float(x) / divisions, v = float(y) / divisions;
        const matter::Float3 p{-.4f + .8f*u, -.4f + .8f*v, 0};
        const float su = (4 + 120*u)/128, sv = (4 + 120*v)/128;
        part.vertices.push_back({p, {0,0,1}, {1,1,1,0}, {su,sv,1,1}, 5, {}});
        mesh.positions.insert(mesh.positions.end(), {p.x,p.y,p.z});
        mesh.normals.insert(mesh.normals.end(), {0,0,1});
        mesh.surface_uvs.insert(mesh.surface_uvs.end(), {su,sv});
        mesh.material_ids.push_back(5); mesh.surface_weights.push_back(255);
    }
    for (int y = 0; y < divisions; ++y) for (int x = 0; x < divisions; ++x) {
        const uint32_t a = y*(divisions+1)+x, b = a+1, d = a+divisions+1, c = d+1;
        part.indices.insert(part.indices.end(), {a,b,c,a,c,d});
    }
    mesh.indices = part.indices; mesh.vertex_count = uint32_t(part.vertices.size());
    auto& rung = part.lod_charts[0];
    rung.atlas_w = rung.atlas_h = 128; rung.charts.resize(1);
    auto& chart = rung.charts[0];
    chart.origin[0] = chart.origin[1] = -.4f; chart.origin[2] = 0;
    chart.rect_x = chart.rect_y = 0; chart.first_tri = 0;
    chart.tri_count = uint32_t(part.indices.size()/3);
    rung.tri_order.resize(chart.tri_count);
    for (uint32_t i = 0; i < chart.tri_count; ++i) rung.tri_order[i] = i;
    auto& cluster = part.clusters[0];
    cluster.aabb_min = {-.4f,-.4f,0}; cluster.aabb_max = {.4f,.4f,0};
    cluster.radius = std::sqrt(.32f);
    cluster.lods[0] = {0,uint32_t(part.indices.size()),0,0};
    part.surface_local_to_world[3] = center_x;
    part.surface_local_to_world[11] = -2;
    return part;
}

template<class Frame, class Settle>
void run(viewer::VkSceneRenderer& renderer, const viewer::VkScenePart& base,
         matter::CameraDesc& camera, viewer::FrameMatrices& matrices,
         matter::TilesetPomSettings& pom, viewer::RtSurfaceHit& hit,
         uint32_t& invalid, Frame&& frame, Settle&& settle,
         uint32_t width, uint32_t height) {
    std::string error;
    const auto identity = [] {
        matter::Mat4f m{}; m.m[0]=m.m[5]=m.m[10]=m.m[15]=1; return m;
    };
    const auto sub = [](matter::Float3 a,matter::Float3 b) {
        return matter::Float3{a.x-b.x,a.y-b.y,a.z-b.z};
    };
    const auto length = [](matter::Float3 a) { return std::sqrt(a.x*a.x+a.y*a.y+a.z*a.z); };
    const auto point = [&](float depth) {
        return viewer::unproject_ndc(matrices.clip_to_world,
            {(160.5f/width)*2-1,1-(100.5f/height)*2,depth});
    };
    const auto raster = [&](bool enabled) {
        pom.enabled=enabled; renderer.set_tileset_pom_settings(pom);
        for (int i=0;i<4;++i) { glfwPollEvents(); frame(false,{},{},0,0); }
        viewer::VkRasterPixel p{};
        CHECK(renderer.readback_raster_pixel(160,100,p,error),error.c_str());
        CHECK(p.material_index==5,"sector POM: raster sample resolves the authored carrier");
        return p;
    };
    for (int recipe=0;recipe<2;++recipe) {
        const float slope=recipe ? .01f : 0;
        auto reference=base;
        reference.part_hash=0x5ec70000u+recipe*16;
        reference.surface_world_anchored=1;
        reference.surface_materials={5};
        reference.surface_tape_hash=0x5ec71000u+recipe;
        // World-space channels are identical for every layout/local frame.
        reference.surface_tape_text=
            "input wx\nconst 0.4\nmul r0 r1\nconst 0.5\nadd r2 r3\n"
            "const 0.2\nmul r0 r5\nconst 0.6\nadd r6 r7\n"
            "const 0\nconst 1\nconst -0.03\nconst "+std::to_string(slope)+
            "\nmul r0 r12\nadd r11 r13\nmaterial 5 r10\n"
            "source 1 r4 r1 r5 r8 r9 r10 r14 -0.08 0\n";
        for (auto& v:reference.vertices) v.material_index=5;
        auto& mesh=reference.lod_chart_meshes[0];
        mesh.material_ids.assign(mesh.vertex_count,5);
        mesh.surface_weights.assign(mesh.vertex_count,255);
        for (int layout=0;layout<3;++layout) {
            const char* name=layout==0 ? "reference" : layout==1 ? "split" : "split-unequal";
            std::vector<uint64_t> hashes;
            std::vector<viewer::VkSceneInstance> instances;
            if (layout==0) {
                CHECK(renderer.ensure_part(reference,error)>=0,error.c_str());
                hashes.push_back(reference.part_hash);
                instances.push_back({reference.part_hash,identity(),1});
            } else for (int side=0;side<2;++side) {
                const float center=side ? .4f : -.4f;
                auto part=half(reference,reference.part_hash+layout*2+side,center,
                               layout==2 && side==1 ? 4 : 1);
                CHECK(renderer.ensure_part(part,error)>=0,error.c_str());
                hashes.push_back(part.part_hash);
                auto transform=identity();transform.m[3]=center;transform.m[11]=-2;
                instances.push_back({part.part_hash,transform,uint32_t(side+1)});
            }
            CHECK(renderer.update_instances(instances,error),error.c_str());
            CHECK(renderer.set_vt_surface_connections(layout==0 ? std::vector<vt::VtSurfaceConnectionPair>{} :
                std::vector<vt::VtSurfaceConnectionPair>{{hashes[0],hashes[1],0x5ec70,0,0}},error),error.c_str());
            camera.position={-3,0,0};camera.target={0,0,-2};
            CHECK(viewer::build_frame_matrices(camera,width,height,matrices,error),error.c_str());
            pom.enabled=true;renderer.set_tileset_pom_settings(pom);settle();
            uint32_t slots[2]{};
            for (float eye:{-3.f,3.f}) {
                float raster_error=0,ray_error=0,color_error=0,normal_error=0,roughness_error=0;
                float flat_error=0; int crossed=0,flat_raster=0,flat_ray=0;
                for (float target:{-.08f,-.045f,-.025f,-.012f,-.003f,.003f,.012f,.025f,.045f,.08f}) {
                    camera.position={eye,0,0};camera.target={target,0,-2};
                    CHECK(viewer::build_frame_matrices(camera,width,height,matrices,error),error.c_str());
                    const auto plain=raster(false); const auto proxy=point(plain.depth);
                    frame(true,camera.position,sub(proxy,camera.position),0,0);
                    CHECK(hit.valid && hit.vt_applied && !invalid,"sector POM: flat RT sample resolves VT");
                    flat_error=std::max(flat_error,length(sub(hit.position,proxy)));
                    slots[proxy.x<0 ? 0 : 1]=hit.vt_slot;
                    auto d=sub(proxy,camera.position);const float len=length(d);
                    d={d.x/len,d.y/len,d.z/len};
                    const float t=(.03f-slope*proxy.x)/(-d.z+slope*d.x);
                    const matter::Float3 expected{proxy.x+d.x*t,proxy.y+d.y*t,proxy.z+d.z*t};
                    if (proxy.x*expected.x<0)++crossed;
                    const auto displaced=raster(true);const auto actual=point(displaced.depth);
                    frame(true,camera.position,sub(proxy,camera.position),0,0);
                    CHECK(hit.valid && hit.vt_applied && !invalid,"sector POM: displaced RT sample resolves VT");
                    if (length(sub(actual,proxy))<.0001f)++flat_raster;
                    if (length(sub(hit.position,proxy))<.0001f)++flat_ray;
                    raster_error=std::max(raster_error,length(sub(actual,expected)));
                    ray_error=std::max(ray_error,length(sub(hit.position,expected)));
                    const float red=.5f+.4f*expected.x, rough=.6f+.2f*expected.x;
                    color_error=std::max({color_error,std::abs(displaced.albedo.x-red),std::abs(hit.vt_albedo.x-red)});
                    roughness_error=std::max(roughness_error,std::abs(displaced.orm.x-rough));
                    const float nx=-slope/std::sqrt(1+slope*slope);
                    normal_error=std::max({normal_error,std::abs(displaced.normal.x-nx),std::abs(hit.vt_normal.x-nx)});
                    std::printf("VT_SECTOR_SAMPLE recipe=%d layout=%s eye=%g proxy_x=%.7f expected_x=%.7f raster_z=%.7f rt_z=%.7f slot=%u mip=%.0f/%.0f\n",
                        recipe,name,eye,proxy.x,expected.x,actual.z,hit.position.z,hit.vt_slot,hit.vt_desired_mip,hit.vt_mapped_mip);
                }
                std::printf("VT_SECTOR_SEAM recipe=%d layout=%s eye=%g crossed=%d flat_raster=%d flat_rt=%d flat_error=%.8f raster_error=%.8f rt_error=%.8f color_error=%.7f roughness_error=%.7f normal_error=%.7f slots=%u,%u\n",
                    recipe,name,eye,crossed,flat_raster,flat_ray,flat_error,raster_error,ray_error,color_error,roughness_error,normal_error,slots[0],slots[1]);
                CHECK(crossed>=3,"sector POM: multiple rays must cross the owner boundary before reaching the surface");
                CHECK(flat_error<.0001f,"sector POM: split proxy geometry has no gap or RT alignment error");
                CHECK(slots[0] && slots[1] && ((slots[0]==slots[1])==(layout==0)),
                      "sector POM: actual GPU hits prove joined versus independent VT owners");
                CHECK(raster_error<.0003f,"sector POM: raster depth matches the continuous analytic surface");
                CHECK(ray_error<.0003f,"sector POM: RT depth matches the continuous analytic surface");
                CHECK(color_error<.015f && roughness_error<.015f && normal_error<.015f,
                      "sector POM: displaced color, roughness and normal match the world-space source");
            }
            CHECK(renderer.update_instances({},error),error.c_str());
            for (uint64_t hash:hashes)renderer.release_part(hash);
        }
    }
}
} // namespace vt_sector_seam_tests
