#pragma once

// Immutable CPU inputs for preparation jobs. Geometry is shared across surface
// edits; each published context binds pointers into its own retained inputs.
// These objects contain no GPU handles and may be released on a worker thread.
#include "vt_types.h"
#include "vt_finite_sources.h"

#include <string>
#include <utility>

namespace vt {

struct VtGeometryInputs {
    chart_atlas::ChartAtlasRung atlas;
    std::vector<float> positions, normals, surface_uvs, material_table;
    std::vector<uint32_t> material_ids, indices;
    std::vector<uint8_t> tint_rgba;
};

struct VtSurfaceInputs {
    std::vector<uint8_t> weights;
    std::vector<uint32_t> materials;
    std::string tape_text;
    std::vector<uint16_t> lanes;
    uint64_t tape_hash = 0;
    uint32_t lane_count = 0;
    bool has_tape_text = false;
    std::shared_ptr<const VtFiniteSources> finite_sources;
    std::vector<uint32_t> finite_source_ids;

    size_t bytes() const {
        return weights.size() + materials.size() * sizeof(uint32_t) +
               tape_text.size() + lanes.size() * sizeof(uint16_t) +
               finite_source_ids.size()*sizeof(uint32_t) + (finite_sources ? finite_sources->bytes() : 0);
    }
};

struct VtPartSnapshot {
    std::shared_ptr<const VtGeometryInputs> geometry;
    std::shared_ptr<const VtSurfaceInputs> surface;
    VtPartContext context;

    // The caller supplies an already chosen tape hash/version. Residency folds
    // the evaluator version into that hash before capture, as before.
    static std::shared_ptr<const VtPartSnapshot> capture(
        const chart_atlas::ChartAtlasRung& atlas, const VtPartContext& input) {
        auto mesh = std::make_shared<VtGeometryInputs>();
        mesh->atlas = atlas;
        const size_t vertices = input.vertex_count;
        copy(mesh->positions, input.positions, vertices * 3);
        copy(mesh->normals, input.normals, vertices * 3);
        copy(mesh->surface_uvs, input.surface_uvs, vertices * 2);
        copy(mesh->material_table, input.material_table,
             static_cast<size_t>(input.material_count) * input.material_stride);
        copy(mesh->material_ids, input.material_ids, vertices);
        copy(mesh->tint_rgba, input.tint_rgba, vertices * 4);
        copy(mesh->indices, input.indices, static_cast<size_t>(input.triangle_count) * 3);
        return bind(std::move(mesh), capture_surface(input), input);
    }

    // input carries the same geometry metadata and new surface inputs. All
    // geometry pointer fields are rebound to the shared immutable mesh.
    std::shared_ptr<const VtPartSnapshot> with_surface(const VtPartContext& input) const {
        return bind(geometry, capture_surface(input), input);
    }

    bool owns_context_inputs() const {
        return geometry && surface && context.atlas == &geometry->atlas &&
            context.positions == data(geometry->positions) &&
            context.normals == data(geometry->normals) &&
            context.surface_uvs == data(geometry->surface_uvs) &&
            context.material_table == data(geometry->material_table) &&
            context.material_ids == data(geometry->material_ids) &&
            context.tint_rgba == data(geometry->tint_rgba) &&
            context.indices == data(geometry->indices) &&
            context.surface_weights == data(surface->weights) &&
            context.surface_materials == data(surface->materials) &&
            context.surface_lanes == data(surface->lanes) &&
            context.finite_sources == surface->finite_sources &&
            context.finite_source_ids == data(surface->finite_source_ids) &&
            context.surface_tape_text == (surface->has_tape_text ? surface->tape_text.c_str() : nullptr);
    }

private:
    template<class T> static void copy(std::vector<T>& out, const T* input, size_t count) {
        if (input && count) out.assign(input, input + count);
    }
    template<class T> static const T* data(const std::vector<T>& input) {
        return input.empty() ? nullptr : input.data();
    }
    static std::shared_ptr<const VtSurfaceInputs> capture_surface(const VtPartContext& input) {
        auto result = std::make_shared<VtSurfaceInputs>();
        result->finite_sources=input.finite_sources;
        copy(result->finite_source_ids,input.finite_source_ids,input.vertex_count);
        const uint32_t count = input.surface_material_count;
        if (!count || count > 8 || !input.surface_weights || !input.surface_materials ||
            !input.vertex_count) return result;
        copy(result->weights, input.surface_weights, static_cast<size_t>(input.vertex_count) * count);
        copy(result->materials, input.surface_materials, count);
        result->tape_hash = input.surface_tape_hash;
        result->has_tape_text = input.surface_tape_text != nullptr;
        if (result->has_tape_text) result->tape_text = input.surface_tape_text;
        if (input.surface_lanes && input.surface_lane_count) {
            copy(result->lanes, input.surface_lanes,
                 static_cast<size_t>(input.vertex_count) * input.surface_lane_count);
            result->lane_count = input.surface_lane_count;
        }
        return result;
    }
    static std::shared_ptr<const VtPartSnapshot> bind(
        std::shared_ptr<const VtGeometryInputs> mesh,
        std::shared_ptr<const VtSurfaceInputs> fields, const VtPartContext& input) {
        auto result = std::make_shared<VtPartSnapshot>();
        result->geometry = std::move(mesh);
        result->surface = std::move(fields);
        auto& ctx = result->context;
        ctx = input;
        const auto& geo = *result->geometry;
        const auto& surf = *result->surface;
        ctx.atlas = &geo.atlas;
        ctx.positions = data(geo.positions);
        ctx.normals = data(geo.normals);
        ctx.surface_uvs = data(geo.surface_uvs);
        ctx.material_table = data(geo.material_table);
        ctx.material_ids = data(geo.material_ids);
        ctx.tint_rgba = data(geo.tint_rgba);
        ctx.indices = data(geo.indices);
        ctx.triangle_count = static_cast<uint32_t>(geo.indices.size() / 3);
        ctx.surface_weights = data(surf.weights);
        ctx.surface_materials = data(surf.materials);
        ctx.surface_material_count = static_cast<uint32_t>(surf.materials.size());
        ctx.surface_tape_hash = surf.tape_hash;
        ctx.surface_tape_text = surf.has_tape_text ? surf.tape_text.c_str() : nullptr;
        ctx.surface_lanes = data(surf.lanes);
        ctx.surface_lane_count = surf.lane_count;
        ctx.finite_sources=surf.finite_sources;
        ctx.finite_source_ids=data(surf.finite_source_ids);
        return result;
    }
};

} // namespace vt
