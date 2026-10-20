#include "portable_scene.h"

#include "vulkan_device_binding.h"
#include "../authoring/engine/legacy_part_route.h"
#include "export/export_image.h"
#include "render/vk_scene_renderer.h"
#include "matter/log.h"
#include "matter_math.h"

#include <matter/graphics/graphics.h>
#include <matter/graphics/vulkan_raster.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>

namespace matter::graphics_adapter {
namespace {
namespace d = matter::contracts;
namespace s = matter::services;
namespace mg = matter::graphics;

const s::JobEnvelope kJob{"matter.engine.graphics", "1", 1};

std::string join(const std::vector<d::Diagnostic>& diagnostics) {
    std::string out;
    for (const auto& x : diagnostics)
        out += std::string(out.empty() ? "" : "; ") + d::code_name(x.code) + ": " + x.message;
    return out;
}

mm::Mat4 to_mm(const Mat4f& m) {
    mm::Mat4 out;
    std::copy(std::begin(m.m), std::end(m.m), out.m);
    return out;
}

// PFM, bottom row first per the format; `get` yields `channels` floats.
template <class Get>
bool write_pfm(const std::string& path, uint32_t w, uint32_t h, int channels, Get get) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "%s\n%u %u\n-1.0\n", channels == 3 ? "PF" : "Pf", w, h);
    std::vector<float> row(size_t(w) * channels);
    for (uint32_t y = h; y-- > 0;) {
        for (uint32_t x = 0; x < w; ++x) get(size_t(y) * w + x, &row[size_t(x) * channels]);
        std::fwrite(row.data(), sizeof(float), row.size(), f);
    }
    return std::fclose(f) == 0;
}

struct Upload {
    std::shared_ptr<const s::AssetProduct> product;
    std::shared_ptr<const s::UploadTicket> ticket; // Until ready.
    s::GraphicsHandle handle{};
    bool ready = false;
};
} // namespace

struct PortableScene::Impl {
    VulkanDevice& engine_device;
    mg::VulkanDeviceBinding binding;
    std::shared_ptr<mg::VulkanResourceDevice> device;
    mg::SceneBackend* scene = nullptr; // Owned by graphics.
    std::unique_ptr<mg::Graphics> graphics;
    std::unique_ptr<mg::VulkanRaster> raster;
    std::map<uint64_t, Upload> uploads;        // By part hash.
    std::set<uint64_t> refused;                // Logged once, legacy-only.
    std::set<std::string> refused_placements;  // Instance owners, likewise.
    std::map<std::string, s::SceneInstance> placed;
    std::vector<viewer::VkSceneInstance> admitted;
    uint64_t revision = 0;
    uint64_t products_seen = 0;                // retained_generation() last published.
    std::string capture_prefix;
    uint32_t capture_after = 30, stable_frames = 0;
    bool captured = false;

    explicit Impl(VulkanDevice& d) : engine_device(d) {
        if (const char* prefix = std::getenv("MATTER_PORTABLE_SCENE_CAPTURE")) capture_prefix = prefix;
        if (const char* frames = std::getenv("MATTER_PORTABLE_SCENE_CAPTURE_FRAMES"))
            capture_after = uint32_t(std::strtoul(frames, nullptr, 10));
    }
    ~Impl() {
        raster.reset();
        if (graphics) graphics->dispose();
        if (device) device->drain();
    }

    bool lease(std::string& err) {
        if (graphics) return true;
        if (!vulkan_device_binding(engine_device, binding, err)) return false;
        device = std::make_shared<mg::VulkanResourceDevice>(binding);
        scene = new mg::SceneBackend(device);
        graphics = std::make_unique<mg::Graphics>(std::unique_ptr<mg::Backend>(scene));
        return true;
    }

    void refuse(uint64_t hash, const std::string& why) {
        if (refused.insert(hash).second)
            MATTER_LOGE("graphics", "portable part %016llx stays legacy-only: %s",
                        (unsigned long long)hash, why.c_str());
        uploads.erase(hash);
    }

    // Starts or advances one hash's upload; true once its handle is ready.
    bool resident(uint64_t hash, const std::shared_ptr<const s::AssetProduct>& product) {
        if (refused.count(hash)) return false;
        auto [it, fresh] = uploads.try_emplace(hash);
        Upload& up = it->second;
        if (fresh) {
            for (const auto& i : product->document.instances)
                if (i.target != d::InstanceTarget::mesh) {
                    refuse(hash, "assembly instance " + i.id.value + " needs the child-part adapter");
                    return false;
                }
            const std::string material = authoring_adapter::legacy_material_refusal(product->document);
            if (!material.empty()) { refuse(hash, material); return false; }
            up.product = product;
            auto ticket = graphics->upload(kJob, {s::AssetOwner(product, &product->document),
                                                  product->resources, {}}, {});
            if (!ticket.ok()) { refuse(hash, "upload: " + join(ticket.diagnostics())); return false; }
            up.ticket = ticket.value();
        }
        if (up.ready) return true;
        auto polled = graphics->poll(kJob, *up.ticket);
        if (!polled.ok()) { refuse(hash, "upload poll: " + join(polled.diagnostics())); return false; }
        if (polled.value()->state != s::UploadState::ready) return false;
        up.handle = polled.value()->resource;
        up.ready = true;
        (void)graphics->release_ticket(*up.ticket);
        up.ticket.reset();
        return true;
    }

    // The admitted instances, expanded to their asset instances in part space.
    bool publish(std::string& err) {
        std::map<std::string, s::SceneInstance> next;
        std::set<uint64_t> used;
        // Residency is settled once per part per pass: an upload that turns
        // ready mid-pass must not publish only the instances after it, since
        // nothing re-runs publish() once no upload is pending.
        std::map<uint64_t, bool> ready;
        for (size_t index = 0; index < admitted.size(); ++index) {
            const auto& inst = admitted[index];
            const auto product = authoring_adapter::retained_asset(inst.part_hash);
            if (!product) {
                // A portable hash that resolved but could not be built is refused
                // with its diagnostic; anything else is legacy content.
                const std::string why = authoring_adapter::retained_refusal(inst.part_hash);
                if (!why.empty()) refuse(inst.part_hash, "no product: " + why);
                continue;
            }
            used.insert(inst.part_hash);
            auto r = ready.find(inst.part_hash);
            if (r == ready.end()) r = ready.emplace(inst.part_hash, resident(inst.part_hash, product)).first;
            if (!r->second) continue;
            const s::GraphicsHandle handle = uploads[inst.part_hash].handle;
            const mm::Mat4 object_to_world = to_mm(inst.object_to_world);
            char owner[40];
            std::snprintf(owner, sizeof owner, "%016llx/",
                          (unsigned long long)(inst.instance_id ? inst.instance_id : index));
            // The scene takes affine placements only; any other one stays with
            // the legacy renderer rather than failing the whole update.
            if (const auto placement = d::validate_transform(object_to_world); !placement.ok()) {
                if (refused_placements.insert(owner).second)
                    MATTER_LOGE("graphics", "portable instance %.16s stays legacy-only: %s", owner,
                                join(placement.diagnostics).c_str());
                continue;
            }
            for (const auto& child : product->document.instances) {
                mm::Mat4 part_space;
                if (!authoring_adapter::compose_instance_transform(product->document, child, part_space)) {
                    refuse(inst.part_hash, "broken parent chain");
                    break;
                }
                const std::string id = owner + child.id.value;
                next[id] = {{id}, child.prototype, handle, mm::multiply(object_to_world, part_space)};
            }
        }
        s::SceneUpdate update{revision + 1, {}, {}};
        for (const auto& [id, instance] : next) {
            const auto was = placed.find(id);
            if (was == placed.end() || was->second.resource.slot != instance.resource.slot ||
                was->second.resource.generation != instance.resource.generation ||
                !std::equal(std::begin(was->second.world_transform.m), std::end(was->second.world_transform.m),
                            std::begin(instance.world_transform.m)))
                update.upsert.push_back(instance);
        }
        for (const auto& [id, instance] : placed)
            if (!next.count(id)) update.remove.push_back(instance.id);
        if (!update.upsert.empty() || !update.remove.empty()) {
            const auto status = graphics->update_scene(kJob, update);
            if (!status.ok()) { err = "portable scene update: " + join(status.diagnostics()); return false; }
            revision = update.scene_revision;
            placed = std::move(next);
            stable_frames = 0;
            captured = false;
        }
        // Retire products no admitted instance references any more.
        for (auto it = uploads.begin(); it != uploads.end();) {
            if (used.count(it->first)) { ++it; continue; }
            if (it->second.ticket) (void)graphics->release_ticket(*it->second.ticket);
            if (it->second.ready) (void)graphics->release_resource(it->second.handle);
            it = uploads.erase(it);
        }
        graphics->collect_garbage();
        return true;
    }

    bool capture(const PortableView& view, std::string& err);
};

PortableScene::PortableScene(VulkanDevice& device) : impl_(std::make_unique<Impl>(device)) {}
PortableScene::~PortableScene() = default;

bool PortableScene::update(const std::vector<viewer::VkSceneInstance>& instances,
                           bool instances_changed, const PortableView& view, std::string& err) {
    if (!impl_->lease(err)) return false;
    bool pending = false;
    for (const auto& [hash, up] : impl_->uploads) pending |= !up.ready;
    if (instances_changed) impl_->admitted = instances;
    // A product retained after the instances were admitted (or refused since)
    // republishes the unchanged list. Read before publishing, so one landing
    // mid-pass is seen on the next update.
    const uint64_t products = authoring_adapter::retained_generation();
    const bool late = products != impl_->products_seen;
    impl_->products_seen = products;
    if ((instances_changed || pending || late) && !impl_->publish(err)) return false;
    s::CameraState camera;
    std::copy(std::begin(view.world_to_view.m), std::end(view.world_to_view.m), camera.view.m);
    std::copy(std::begin(view.view_to_clip.m), std::end(view.view_to_clip.m), camera.projection.m);
    s::LightState sun;
    sun.id = {"sun"};
    sun.direction = {view.sun_direction.x, view.sun_direction.y, view.sun_direction.z};
    sun.color = {view.sun_color.x, view.sun_color.y, view.sun_color.z};
    const auto set_camera = impl_->graphics->set_camera(kJob, camera);
    const auto set_lights = impl_->graphics->set_lights(kJob, {sun});
    if (!set_camera.ok() || !set_lights.ok()) {
        err = "portable scene view: " + join(set_camera.ok() ? set_lights.diagnostics() : set_camera.diagnostics());
        return false;
    }
    if (impl_->capture_prefix.empty() || impl_->captured || impl_->placed.empty() || pending) return true;
    if (++impl_->stable_frames < impl_->capture_after) return true;
    impl_->captured = true;
    return impl_->capture(view, err);
}

// Rasterizes this frame's SceneFrame offscreen and records what was drawn.
bool PortableScene::Impl::capture(const PortableView& view, std::string& err) {
    mg::SceneFrame frame;
    const auto framed = scene->frame(frame);
    if (!framed.ok()) { err = "portable scene frame: " + join(framed.diagnostics); return false; }
    if (!raster) raster = std::make_unique<mg::VulkanRaster>(binding);
    mg::RasterOutput out;
    const auto drawn = raster->render(frame, view.width, view.height, out);
    if (!drawn.ok()) { err = "portable scene raster: " + join(drawn.diagnostics); return false; }
    const uint32_t w = out.albedo.width, h = out.albedo.height;
    std::vector<size_t> pixels(frame.visible.size());
    matter_export::ExportImage albedo{w, h, 3, std::vector<uint8_t>(size_t(w) * h * 3), 0};
    for (size_t i = 0; i < size_t(w) * h; ++i) {
        const auto id = out.identity.pixels[i];
        if (id[1] < pixels.size()) ++pixels[id[1]];
        for (int c = 0; c < 3; ++c)
            albedo.texels[i * 3 + c] = uint8_t(std::lround(std::clamp(out.albedo.pixels[i][c], 0.0f, 1.0f) * 255.0f));
    }
    const std::string& p = capture_prefix;
    std::string write_error;
    if (!matter_export::write_image_file(albedo, matter_export::TextureFormat::Png, p + ".albedo.png", write_error) ||
        !write_pfm(p + ".normal.pfm", w, h, 3, [&](size_t i, float* o) { std::copy_n(out.normal.pixels[i].data(), 3, o); }) ||
        !write_pfm(p + ".depth.pfm", w, h, 1, [&](size_t i, float* o) { *o = out.depth.pixels[i][0]; }) ||
        !write_pfm(p + ".identity.pfm", w, h, 3, [&](size_t i, float* o) {
            const auto id = out.identity.pixels[i];
            o[0] = id[0] == UINT32_MAX ? -1.0f : float(id[0]);
            o[1] = id[1] == UINT32_MAX ? -1.0f : float(id[1]);
            o[2] = 0.0f;
        })) {
        err = "portable scene capture: cannot write " + p + " " + write_error;
        return false;
    }
    FILE* json = std::fopen((p + ".json").c_str(), "wb");
    if (!json) { err = "portable scene capture: cannot write " + p + ".json"; return false; }
    std::fprintf(json, "{\n  \"revision\": %llu,\n  \"width\": %u,\n  \"height\": %u,\n  \"refused\": [",
                 (unsigned long long)revision, w, h);
    for (auto it = refused.begin(); it != refused.end(); ++it)
        std::fprintf(json, "%s\"%016llx\"", it == refused.begin() ? "" : ", ", (unsigned long long)*it);
    std::fprintf(json, "],\n  \"refused_placements\": [");
    for (auto it = refused_placements.begin(); it != refused_placements.end(); ++it)
        std::fprintf(json, "%s\"%s\"", it == refused_placements.begin() ? "" : ", ", it->c_str());
    std::fprintf(json, "],\n  \"assets\": [");
    bool first = true;
    for (const auto& [hash, up] : uploads) {
        std::fprintf(json, "%s\n    {\"part_hash\": \"%016llx\", \"ready\": %s, \"product\": \"%p\", \"meshes\": %zu}",
                     first ? "" : ",", (unsigned long long)hash, up.ready ? "true" : "false",
                     (const void*)up.product.get(), up.product->document.meshes.size());
        first = false;
    }
    std::fprintf(json, "\n  ],\n  \"draws\": [");
    for (size_t i = 0; i < frame.visible.size(); ++i) {
        const auto& v = frame.visible[i];
        const auto& mesh = v.asset->meshes[v.mesh];
        const std::string material = mesh.submeshes.empty() ? "" : mesh.submeshes.front().material.value;
        std::fprintf(json, "%s\n    {\"instance\": \"%s\", \"raster_identity\": \"%s\", \"prototype\": \"%s\", "
                           "\"material\": \"%s\", \"mirrored\": %s, \"winding\": \"%s\", \"pixels\": %zu, "
                           "\"resident_bytes\": %llu}",
                     i ? "," : "", v.instance.value.c_str(), out.instances[i].instance.value.c_str(),
                     mesh.prototype.value.c_str(), material.c_str(), v.mirrored ? "true" : "false",
                     mesh.winding == d::Winding::counterclockwise ? "ccw" : "cw", pixels[i],
                     (unsigned long long)v.asset->resident_bytes());
    }
    std::fprintf(json, "\n  ],\n  \"observations\": [");
    const auto observed = graphics->observations();
    for (size_t i = 0; i < observed.size(); ++i)
        std::fprintf(json, "%s\n    {\"instance\": \"%s\", \"resident_bytes\": %llu}", i ? "," : "",
                     observed[i].instance.value.c_str(), (unsigned long long)observed[i].resident_bytes);
    std::fprintf(json, "\n  ],\n  \"materials\": [");
    for (size_t i = 0; i < out.materials.size(); ++i) {
        const auto& m = out.materials[i];
        std::fprintf(json, "%s[%.6f, %.6f, %.6f]", i ? ", " : "", m.base_roughness[0], m.base_roughness[1],
                     m.base_roughness[2]);
    }
    std::fprintf(json, "]\n}\n");
    std::fclose(json);
    MATTER_LOGI("graphics", "portable scene captured %zu draws at revision %llu to %s", frame.visible.size(),
                (unsigned long long)revision, p.c_str());
    return true;
}

} // namespace matter::graphics_adapter
