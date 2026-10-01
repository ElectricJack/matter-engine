// MatterEditor/src/viewport_capture.cpp — see viewport_capture.h.

#include "viewport_capture.h"

#include "camera_calibration.h"
#include "view_projection.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <utility>

namespace viewer::capture {
namespace {

using JsonValue = matter::jsondoc::Value;
using agent::json::array;
using agent::json::boolean;
using agent::json::decimal;
using agent::json::number;
using agent::json::object;
using agent::json::string;
using agent::json::unavailable;
using agent::json::vec3;

JsonValue rect_json(const Rect& rect) {
    JsonValue value = object();
    value.set("x", number(rect.x));
    value.set("y", number(rect.y));
    value.set("width", number(rect.width));
    value.set("height", number(rect.height));
    return value;
}

}  // namespace

ReadinessReport evaluate_readiness(const ReadinessInput& input) {
    ReadinessReport result;
    const auto block = [&](bool condition, const char* reason) {
        if (condition) result.blockers.emplace_back(reason);
    };
    block(!input.production_view, "production_view_unavailable");
    block(!input.scene_ready, "scene_not_ready");
    block(!input.gpu_jobs_idle, "gpu_jobs_pending");
    block(!input.visible_streaming_known, "visible_streaming_unknown");
    block(input.visible_sectors_pending != 0, "visible_sectors_pending");
    block(input.visible_refinement_pending != 0, "visible_refinement_pending");
    block(!input.detail.observed, "visible_draw_census_unavailable");
    block(input.detail.visible_instances == 0, "no_visible_object_pixels");
    block(input.detail.unmatched_tokens != 0, "unmatched_visible_identity");
    block(input.detail.identities.size() != input.detail.visible_instances,
          "identity_map_incomplete");
    block(input.detail.identities.size() > 2048, "identity_map_truncated");
    block(input.detail.missing_draws != 0, "visible_draw_missing");
    block(input.detail.coarse_draws != 0, "visible_vg_coarser_than_requested");
    block(input.detail.missing_blas != 0, "visible_blas_missing");
    block(input.detail.missing_vt != 0, "visible_vt_missing");
    block(input.vt_queue_depth != 0 || input.vt_dirty_pages != 0,
          "vt_content_pending");
    block(input.vt_rejected_variants != 0, "vt_variant_rejected");
    block(input.require_rt && (!input.rt_available || !input.rt_effective),
          "native_rt_unavailable");
    block(input.stable_camera_frames < 3, "temporal_settle_pending");
    result.ready = result.blockers.empty();
    return result;
}

Rect viewport_in_framebuffer(const Geometry& geometry) {
    return Rect{geometry.viewport_logical.x * geometry.framebuffer_scale_x,
                geometry.viewport_logical.y * geometry.framebuffer_scale_y,
                geometry.viewport_logical.width * geometry.framebuffer_scale_x,
                geometry.viewport_logical.height * geometry.framebuffer_scale_y};
}

Rect viewport_in_image(const Geometry& geometry) {
    const Rect framebuffer = viewport_in_framebuffer(geometry);
    return Rect{framebuffer.x - geometry.image_origin_x,
                framebuffer.y - geometry.image_origin_y, framebuffer.width,
                framebuffer.height};
}

bool image_to_viewport_logical(const Geometry& geometry, double image_x,
                               double image_y, double& out_x, double& out_y) {
    const Rect viewport = viewport_in_image(geometry);
    if (viewport.width <= 0.0 || viewport.height <= 0.0) return false;
    if (geometry.framebuffer_scale_x <= 0.0 ||
        geometry.framebuffer_scale_y <= 0.0)
        return false;
    // Half-open on the far edges, exactly like the pick bound: the pixel at
    // x == viewport.x + width belongs to whatever is drawn to the right of the
    // 3D view, not to the last column of it.
    if (image_x < viewport.x || image_y < viewport.y ||
        image_x >= viewport.x + viewport.width ||
        image_y >= viewport.y + viewport.height)
        return false;
    out_x = (image_x - viewport.x) / geometry.framebuffer_scale_x;
    out_y = (image_y - viewport.y) / geometry.framebuffer_scale_y;
    return true;
}

bool viewport_logical_to_image(const Geometry& geometry, double logical_x,
                               double logical_y, double& out_image_x,
                               double& out_image_y) {
    if (geometry.viewport_logical.width <= 0.0 ||
        geometry.viewport_logical.height <= 0.0)
        return false;
    if (logical_x < 0.0 || logical_y < 0.0 ||
        logical_x >= geometry.viewport_logical.width ||
        logical_y >= geometry.viewport_logical.height)
        return false;
    const Rect viewport = viewport_in_image(geometry);
    out_image_x = viewport.x + logical_x * geometry.framebuffer_scale_x;
    out_image_y = viewport.y + logical_y * geometry.framebuffer_scale_y;
    return true;
}

Terminal terminal_for(Resolution resolution) {
    switch (resolution) {
        case Resolution::Captured:
            return {agent::Status::Ok, {}};
        case Resolution::TimedOut:
            return {agent::Status::Timeout,
                    "no frame presented and captured before the request "
                    "deadline; raise timeout_ms or wait for the bake"};
        case Resolution::Abandoned:
            return {agent::Status::NotReady,
                    "the editor stopped presenting frames before the capture "
                    "landed"};
        case Resolution::WriteFailed:
            return {agent::Status::ExecutionFailure,
                    "the presented frame could not be read back and written"};
    }
    return {agent::Status::ExecutionFailure, "unknown capture resolution"};
}

bool Tracker::arm(Request request) {
    if (armed_) return false;
    request_ = std::move(request);
    armed_ = true;
    return true;
}

bool Tracker::owns(const std::string& path) const {
    return armed_ && request_.path == path;
}

bool Tracker::expired(Clock::time_point now) const {
    return armed_ && now >= request_.deadline;
}

Request Tracker::release() {
    if (!armed_) return Request{};
    armed_ = false;
    Request released = std::move(request_);
    request_ = Request{};
    return released;
}

std::vector<Annotation> project_annotations(
    const std::vector<AnnotationInput>& inputs,
    const matter::CameraDesc& camera, const Geometry& geometry) {
    std::vector<Annotation> out;
    out.reserve(inputs.size());

    const Rect viewport = viewport_in_image(geometry);
    const bool viewport_drawable =
        viewport.width > 0.0 && viewport.height > 0.0;

    // The projection is built against the VIEWPORT rectangle, then offset into
    // image pixels: the 3D view fills its own rectangle, not the whole window,
    // so projecting against the framebuffer would put every box in the wrong
    // place whenever the UI is shown.
    projection::Mat4 view_projection{};
    bool camera_usable = viewport_drawable;
    if (camera_usable) {
        const float eye[3] = {camera.position.x, camera.position.y,
                              camera.position.z};
        const float target[3] = {camera.target.x, camera.target.y,
                                 camera.target.z};
        const float up[3] = {camera.up.x, camera.up.y, camera.up.z};
        const float dx = target[0] - eye[0];
        const float dy = target[1] - eye[1];
        const float dz = target[2] - eye[2];
        if (dx * dx + dy * dy + dz * dz < 1e-12f) {
            camera_usable = false;
        } else {
            const float aspect =
                static_cast<float>(viewport.width / viewport.height);
            view_projection = projection::multiply(
                projection::perspective(camera.vertical_fov_radians, aspect,
                                        camera.near_plane, camera.far_plane),
                projection::look_at(eye, target, up));
        }
    }

    for (const AnnotationInput& input : inputs) {
        Annotation annotation;
        annotation.object = input.object;
        annotation.primary = input.primary;
        if (!viewport_drawable) {
            annotation.reason =
                "the capture contains no drawable 3D viewport rectangle";
            out.push_back(std::move(annotation));
            continue;
        }
        if (!camera_usable) {
            annotation.reason =
                "the captured camera has no usable view direction";
            out.push_back(std::move(annotation));
            continue;
        }
        if (!geometry.production_view) {
            annotation.reason =
                "the Part Workbench isolation view owned the captured frame; "
                "the selection describes the production world";
            out.push_back(std::move(annotation));
            continue;
        }
        if (!input.resolved) {
            annotation.reason =
                "the selected object resolved to no bounds in this frame";
            out.push_back(std::move(annotation));
            continue;
        }

        float corners[8][3];
        projection::obb_corners(input.bounds.local_min, input.bounds.local_max,
                                input.bounds.world_matrix, corners);
        double min_x = 0.0, min_y = 0.0, max_x = 0.0, max_y = 0.0;
        bool any = false;
        bool all = true;
        for (const auto& corner : corners) {
            float px = 0.0f;
            float py = 0.0f;
            if (!projection::project_to_pixels(
                    view_projection, static_cast<float>(viewport.width),
                    static_cast<float>(viewport.height),
                    static_cast<float>(viewport.x),
                    static_cast<float>(viewport.y), corner, px, py)) {
                all = false;
                continue;
            }
            if (!any) {
                min_x = max_x = px;
                min_y = max_y = py;
                any = true;
            } else {
                min_x = std::min(min_x, static_cast<double>(px));
                max_x = std::max(max_x, static_cast<double>(px));
                min_y = std::min(min_y, static_cast<double>(py));
                max_y = std::max(max_y, static_cast<double>(py));
            }
        }
        if (!any || !all) {
            // A box with any corner at or behind the eye has no honest
            // screen-aligned rectangle: the missing corners are the ones that
            // would have been widest. Say so rather than draw a box that is
            // confidently wrong.
            annotation.reason =
                "the object's bounding box crosses the camera's eye plane in "
                "this frame";
            out.push_back(std::move(annotation));
            continue;
        }

        const double clamped_min_x = std::max(min_x, viewport.x);
        const double clamped_min_y = std::max(min_y, viewport.y);
        const double clamped_max_x =
            std::min(max_x, viewport.x + viewport.width);
        const double clamped_max_y =
            std::min(max_y, viewport.y + viewport.height);
        if (clamped_max_x <= clamped_min_x || clamped_max_y <= clamped_min_y) {
            annotation.reason =
                "the object projects entirely outside the captured viewport";
            out.push_back(std::move(annotation));
            continue;
        }
        annotation.available = true;
        annotation.clipped = min_x < viewport.x || min_y < viewport.y ||
                             max_x > viewport.x + viewport.width ||
                             max_y > viewport.y + viewport.height;
        annotation.image_rect = Rect{clamped_min_x, clamped_min_y,
                                     clamped_max_x - clamped_min_x,
                                     clamped_max_y - clamped_min_y};
        annotation.viewport_logical_rect =
            Rect{(clamped_min_x - viewport.x) / geometry.framebuffer_scale_x,
                 (clamped_min_y - viewport.y) / geometry.framebuffer_scale_y,
                 annotation.image_rect.width / geometry.framebuffer_scale_x,
                 annotation.image_rect.height / geometry.framebuffer_scale_y};
        out.push_back(std::move(annotation));
    }
    return out;
}

matter::jsondoc::Value capture_result_json(
    const Request& request, const Geometry& geometry,
    const matter::CameraDesc& camera, const agent::Context& context,
    const std::vector<Annotation>* annotations) {
    JsonValue result = object();
    result.set("path", string(request.path));
    // The `shot`/`shot_now` verbs already write this sibling marker; naming it
    // here saves a caller from reconstructing the convention by hand.
    result.set("completion_marker", string(request.path + ".done"));

    JsonValue image = object();
    image.set("format", string("png"));
    image.set("width", number(geometry.image_width));
    image.set("height", number(geometry.image_height));
    JsonValue origin = object();
    origin.set("x", number(geometry.image_origin_x));
    origin.set("y", number(geometry.image_origin_y));
    image.set("framebuffer_origin", std::move(origin));
    result.set("image", std::move(image));

    JsonValue viewport = object();
    viewport.set("logical", rect_json(geometry.viewport_logical));
    viewport.set("framebuffer", rect_json(viewport_in_framebuffer(geometry)));
    viewport.set("image", rect_json(viewport_in_image(geometry)));
    JsonValue scale = object();
    scale.set("x", number(geometry.framebuffer_scale_x));
    scale.set("y", number(geometry.framebuffer_scale_y));
    viewport.set("framebuffer_scale", std::move(scale));
    viewport.set("production_view", boolean(geometry.production_view));
    result.set("viewport", std::move(viewport));

    // How to turn a pixel an agent found in the PNG back into the coordinate
    // viewport.pick takes. Stated as the arithmetic rather than as prose so a
    // client does not have to re-derive it from the three rectangles above.
    JsonValue mapping = object();
    mapping.set("target_space", string("viewport_local_logical_pixels"));
    const Rect viewport_image = viewport_in_image(geometry);
    JsonValue offset = object();
    offset.set("x", number(viewport_image.x));
    offset.set("y", number(viewport_image.y));
    mapping.set("image_offset", std::move(offset));
    JsonValue divisor = object();
    divisor.set("x", number(geometry.framebuffer_scale_x));
    divisor.set("y", number(geometry.framebuffer_scale_y));
    mapping.set("divide_by", std::move(divisor));
    mapping.set("formula",
                string("viewport_x = (image_x - image_offset.x) / "
                             "divide_by.x; same for y"));
    result.set("pick_mapping", std::move(mapping));

    JsonValue camera_json = object();
    camera_json.set("position", vec3(camera.position.x, camera.position.y,
                                       camera.position.z));
    camera_json.set("target",
                    vec3(camera.target.x, camera.target.y, camera.target.z));
    camera_json.set("up", vec3(camera.up.x, camera.up.y, camera.up.z));
    camera_json.set("vertical_fov_radians",
                    number(camera.vertical_fov_radians));
    camera_json.set("near_plane", number(camera.near_plane));
    camera_json.set("far_plane", number(camera.far_plane));
    camera_json.set("intrinsics",
                    camera_calibration::intrinsics(camera, viewport_image.width,
                                                   viewport_image.height,
                                                   viewport_image.x,
                                                   viewport_image.y));
    result.set("camera", std::move(camera_json));

    if (!request.export_channels) {
        result.set("channels", unavailable("export_channels was not requested"));
    } else if (geometry.channels_path.empty()) {
        result.set("channels", unavailable("numeric channel bundle was not written"));
    } else {
        JsonValue channels = object();
        channels.set("available", boolean(true));
        channels.set("path", string(geometry.channels_path));
        channels.set("container", string("MECAP001"));
        channels.set("width", number(geometry.channels_width));
        channels.set("height", number(geometry.channels_height));
        channels.set("origin", string("top_left"));
        channels.set("raster_space", string("internal_render_pixels"));
        channels.set("intrinsics", camera_calibration::intrinsics(
            camera, geometry.channels_width, geometry.channels_height, 0, 0));
        channels.set("identity_semantics", string(
            "uint32 material_index_with_impostor_bit, uint32 frame_local_instance_token; "
            "UINT32_MAX in both lanes is background"));
        channels.set("depth_semantics", string(
            "reversed-Z device depth; 0 is background; linear view depth = "
            "near*far/(near + device_depth*(far-near))"));
        const char* names[] = {"identity", "depth", "normal", "color", "albedo", "orm"};
        const char* formats[] = {"rg32_uint", "r32_float", "rgba16_float",
                                 "rgba16_float", "rgba8_unorm", "rgba16_float"};
        JsonValue planes = array();
        std::uint64_t byte_offset = 16;
        const std::uint64_t pixels =
            std::uint64_t(geometry.channels_width) * geometry.channels_height;
        for (unsigned i = 0; i < matter::EvaluationChannels::Count; ++i) {
            JsonValue plane = object();
            plane.set("name", string(names[i]));
            plane.set("format", string(formats[i]));
            plane.set("offset_bytes", decimal(byte_offset));
            const std::uint64_t bytes = pixels *
                matter::EvaluationChannels::bytes_per_pixel[i];
            plane.set("size_bytes", decimal(bytes));
            planes.arr.push_back(std::move(plane));
            byte_offset += bytes;
        }
        channels.set("planes", std::move(planes));
        JsonValue identities = array();
        const auto& identity_rows = geometry.readiness_input.detail.identities;
        for (size_t i = 0; i < identity_rows.size() && i < 2048; ++i) {
            const auto& identity = identity_rows[i];
            JsonValue row = object();
            row.set("frame_token", number(identity.frame_token));
            row.set("part_hash", decimal(identity.part_hash));
            row.set("kind", string(identity.dynamic_entity
                ? "dynamic_entity" : "static_instance"));
            row.set("resolved", boolean(identity.resolved));
            if (identity.dynamic_entity) {
                row.set("entity_id", decimal(identity.entity_id));
                row.set("entity_generation", number(identity.entity_generation));
            }
            identities.arr.push_back(std::move(row));
        }
        channels.set("identities", std::move(identities));
        channels.set("identities_truncated", boolean(identity_rows.size() > 2048));
        result.set("channels", std::move(channels));
    }

    if (!request.export_channels || geometry.channels_path.empty()) {
        result.set("readiness", unavailable(
            "readiness requires synchronized numeric channels"));
    } else {
        const ReadinessInput& input = geometry.readiness_input;
        const matter::VisibleDetailReport& detail = input.detail;
        JsonValue readiness = object();
        readiness.set("available", boolean(true));
        readiness.set("ready", boolean(geometry.readiness.ready));
        readiness.set("frame_id", decimal(context.frame_id));
        readiness.set("desired_max_lod", number(detail.desired_max_lod));
        readiness.set("visible_instances", number(detail.visible_instances));
        readiness.set("max_selected_lod", number(detail.max_selected_lod));
        readiness.set("unmatched_tokens", number(detail.unmatched_tokens));
        readiness.set("missing_draws", number(detail.missing_draws));
        readiness.set("coarse_draws", number(detail.coarse_draws));
        readiness.set("missing_blas", number(detail.missing_blas));
        readiness.set("missing_vt", number(detail.missing_vt));
        readiness.set("visible_refinement_pending",
                      number(input.visible_refinement_pending));
        readiness.set("visible_sectors_pending",
                      number(input.visible_sectors_pending));
        readiness.set("vt_queue_depth", number(input.vt_queue_depth));
        readiness.set("vt_dirty_pages", number(input.vt_dirty_pages));
        readiness.set("vt_rejected_variants", number(input.vt_rejected_variants));
        readiness.set("rt_available", boolean(input.rt_available));
        readiness.set("rt_effective", boolean(input.rt_effective));
        readiness.set("stable_camera_frames", number(input.stable_camera_frames));
        if (!input.detail_error.empty())
            readiness.set("detail_error", string(input.detail_error));
        JsonValue blockers = array();
        for (const std::string& reason : geometry.readiness.blockers)
            blockers.arr.push_back(string(reason));
        readiness.set("blockers", std::move(blockers));
        JsonValue parts = array();
        for (size_t i = 0; i < detail.parts.size() && i < 128; ++i) {
            const auto& part = detail.parts[i];
            JsonValue row = object();
            row.set("part_hash", decimal(part.part_hash));
            row.set("visible_instances", number(part.visible_instances));
            row.set("vertex_count", number(part.vertex_count));
            row.set("index_count", number(part.index_count));
            row.set("blas_rungs_ready", number(part.blas_rungs_ready));
            row.set("blas_rungs_total", number(part.blas_rungs_total));
            row.set("vt_rungs_active", number(part.vt_rungs_active));
            row.set("vt_rungs_total", number(part.vt_rungs_total));
            parts.arr.push_back(std::move(row));
        }
        readiness.set("parts_truncated", boolean(detail.parts.size() > 128));
        readiness.set("parts", std::move(parts));
        result.set("readiness", std::move(readiness));
    }

    // The envelope's own `context` is a completion-time snapshot; this one is
    // the state of the frame the PNG actually shows. They differ whenever a
    // later frame presented between the capture and the result being written,
    // which is exactly when a caller must not use the newer numbers.
    result.set("captured", agent::context_json(context));
    JsonValue presented = object();
    presented.set("id", decimal(context.frame_id));
    presented.set("view_id", decimal(context.view_id));
    result.set("presented", std::move(presented));

    if (!annotations) {
        result.set("annotations",
                   unavailable("annotate_selection was not requested"));
    } else {
        JsonValue block = object();
        block.set("available", boolean(true));
        block.set("space", string("image_pixels"));
        JsonValue rows;
        rows.kind = JsonValue::Kind::Array;
        for (const Annotation& annotation : *annotations) {
            JsonValue row = object();
            row.set("object", agent::object_identity_json(annotation.object));
            row.set("primary", boolean(annotation.primary));
            row.set("label",
                    string((annotation.object.kind ==
                                          agent::ObjectIdentity::Kind::Entity
                                      ? "entity:"
                                      : "baked_root:") +
                                 std::to_string(annotation.object.id)));
            row.set("available", boolean(annotation.available));
            if (!annotation.available) {
                row.set("reason", string(annotation.reason));
            } else {
                row.set("image_rect", rect_json(annotation.image_rect));
                row.set("viewport_logical_rect",
                        rect_json(annotation.viewport_logical_rect));
                row.set("clipped", boolean(annotation.clipped));
            }
            rows.arr.push_back(std::move(row));
        }
        block.set("objects", std::move(rows));
        result.set("annotations", std::move(block));
    }
    return result;
}

bool parse_arguments(const matter::jsondoc::Value& arguments, Arguments& out,
                     std::string& error) {
    const JsonValue* path = arguments.find("path");
    if (!path || path->kind != JsonValue::Kind::String || path->str.empty()) {
        error = "path must be a non-empty absolute .png path";
        return false;
    }
    out.path = path->str;
    out.annotate = false;
    out.export_channels = false;
    out.desired_max_lod = 0;
    out.require_rt = false;
    if (const JsonValue* annotate = arguments.find("annotate_selection")) {
        if (annotate->kind != JsonValue::Kind::Bool) {
            error = "annotate_selection must be a boolean";
            return false;
        }
        out.annotate = annotate->b;
    }
    if (const JsonValue* channels = arguments.find("export_channels")) {
        if (channels->kind != JsonValue::Kind::Bool) {
            error = "export_channels must be a boolean";
            return false;
        }
        out.export_channels = channels->b;
    }
    if (const JsonValue* lod = arguments.find("desired_max_lod")) {
        if (lod->kind != JsonValue::Kind::Number || !std::isfinite(lod->num) ||
            lod->num < 0 || lod->num > 7 || std::floor(lod->num) != lod->num) {
            error = "desired_max_lod must be an integer from 0 (finest) to 7";
            return false;
        }
        out.desired_max_lod = static_cast<uint32_t>(lod->num);
    }
    if (const JsonValue* rt = arguments.find("require_rt")) {
        if (rt->kind != JsonValue::Kind::Bool) {
            error = "require_rt must be a boolean";
            return false;
        }
        out.require_rt = rt->b;
    }
    if (!out.export_channels && (arguments.find("desired_max_lod") ||
                                 arguments.find("require_rt"))) {
        error = "desired_max_lod and require_rt require export_channels=true";
        return false;
    }
    return true;
}

bool write_channel_bundle(const std::string& path,
                          const matter::EvaluationChannels& channels,
                          std::string& error) {
    const std::uint64_t pixels =
        std::uint64_t(channels.width) * channels.height;
    if (pixels == 0 || pixels > 2048u * 2048u) {
        error = "channel bundle extent is empty or exceeds 2048x2048";
        return false;
    }
    for (unsigned i = 0; i < matter::EvaluationChannels::Count; ++i) {
        if (channels.planes[i].size() !=
            pixels * matter::EvaluationChannels::bytes_per_pixel[i]) {
            error = "channel bundle plane size does not match its format and extent";
            return false;
        }
    }
    { std::ifstream existing(path, std::ios::binary);
      if (existing.good()) {
          error = "channel bundle path already exists; use a unique capture path";
          return false;
      }
    }
    const std::string temporary = path + ".tmp";
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) {
        error = "cannot open temporary channel bundle";
        return false;
    }
    stream.write("MECAP001", 8);
    const auto write_u32 = [&](std::uint32_t value) {
        const char bytes[4] = {char(value), char(value >> 8),
                               char(value >> 16), char(value >> 24)};
        stream.write(bytes, 4);
    };
    write_u32(channels.width);
    write_u32(channels.height);
    for (const auto& plane : channels.planes)
        stream.write(reinterpret_cast<const char*>(plane.data()),
                     static_cast<std::streamsize>(plane.size()));
    stream.close();
    if (!stream || std::rename(temporary.c_str(), path.c_str()) != 0) {
        std::remove(temporary.c_str());
        error = "channel bundle write or atomic rename failed";
        return false;
    }
    return true;
}

}  // namespace viewer::capture
