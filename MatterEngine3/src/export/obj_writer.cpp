// MatterEngine3/src/export/obj_writer.cpp — see obj_writer.h for the file
// layout and the material key mapping.

#include "export/obj_writer.h"

#include "export/export_text.h"
#include "export/texture_bake.h"   // encode_srgb_u8, for the flat Kd/Ke values

#include <algorithm>
#include <cmath>

namespace matter_export {
namespace {

// sRGB-encode a linear channel back to a 0-1 float, for the MTL's flat colour
// keys. three.js's MTLLoader reads Kd/Ke as sRGB, so writing the linear engine
// value there would wash a textured export's fallback colour out.
float srgb_float(float linear) {
    return static_cast<float>(encode_srgb_u8(linear)) / 255.0f;
}

// Blinn-Phong exponent from PBR roughness. There is no exact conversion; this
// is the common alpha = roughness^2, Ns = 2/alpha^2 - 2 mapping, clamped to the
// range renderers actually respect. Only consumers that ignore Pr ever read it.
float specular_exponent(float roughness) {
    const float r = std::min(std::max(roughness, 0.03f), 1.0f);
    const float alpha = r * r;
    const float ns = 2.0f / (alpha * alpha) - 2.0f;
    return std::min(std::max(ns, 1.0f), 1000.0f);
}

void append_vec3(std::string& out, const char* prefix, const float v[3], int decimals) {
    out += prefix;
    out.push_back(' ');
    out += format_fixed(v[0], decimals);
    out.push_back(' ');
    out += format_fixed(v[1], decimals);
    out.push_back(' ');
    out += format_fixed(v[2], decimals);
    out.push_back('\n');
}

std::string map_file_name(const std::string& base_name, MapKind kind,
                          const std::string& extension) {
    return base_name + "_" + map_suffix(kind) + "." + extension;
}

} // namespace

std::string build_obj_text(const ExportModel& model, const std::string& base_name) {
    const ExportMesh& mesh = model.mesh;
    const bool has_uvs = mesh.uvs.size() == static_cast<size_t>(mesh.vertex_count) * 2u;
    const bool has_normals = mesh.normals.size() == static_cast<size_t>(mesh.vertex_count) * 3u;

    std::string out;
    // ~48 bytes per vertex line plus ~40 per face; a generous reserve avoids
    // dozens of reallocations on a 100k-triangle part.
    out.reserve(static_cast<size_t>(mesh.vertex_count) * 120u +
                static_cast<size_t>(mesh.indices.size()) * 16u + 1024u);

    out += "# Wavefront OBJ exported by MatterEngine3\n";
    out += "# part: " + model.name + "\n";
    out += "# resolved_hash: " + format_hex64(model.resolved_hash) + "\n";
    out += "# lod: " + std::to_string(model.lod) + " of " +
           std::to_string(model.lod_count) + "\n";
    out += "# units: metres, Y-up, right-handed, counter-clockwise front faces\n";
    out += "# vertices: " + std::to_string(mesh.vertex_count) + "\n";
    out += "# triangles: " + std::to_string(mesh.triangle_count()) + "\n";
    if (model.charts.chart_count != 0u) {
        out += "# uv_charts: " + std::to_string(model.charts.chart_count) +
               " atlas: " + std::to_string(model.charts.atlas_size) + "\n";
    }
    out += "mtllib " + base_name + ".mtl\n";
    out += "o " + model.name + "\n";

    for (uint32_t v = 0; v < mesh.vertex_count; ++v)
        append_vec3(out, "v", &mesh.positions[static_cast<size_t>(v) * 3u], kPositionDecimals);
    if (has_normals) {
        for (uint32_t v = 0; v < mesh.vertex_count; ++v)
            append_vec3(out, "vn", &mesh.normals[static_cast<size_t>(v) * 3u], kNormalDecimals);
    }
    if (has_uvs) {
        for (uint32_t v = 0; v < mesh.vertex_count; ++v) {
            out += "vt ";
            out += format_fixed(mesh.uvs[static_cast<size_t>(v) * 2u + 0u], kUvDecimals);
            out.push_back(' ');
            out += format_fixed(mesh.uvs[static_cast<size_t>(v) * 2u + 1u], kUvDecimals);
            out.push_back('\n');
        }
    }

    for (const ExportSubmesh& sub : mesh.submeshes) {
        const ExportMaterial& material = model.materials[sub.material_slot];
        out += "usemtl " + material.name + "\n";
        for (uint32_t i = 0; i < sub.index_count; i += 3u) {
            out += "f";
            for (uint32_t k = 0; k < 3u; ++k) {
                // OBJ indices are 1-based and global across the file; the three
                // arrays are parallel, so one number serves all three slots.
                const std::string n =
                    std::to_string(mesh.indices[sub.first_index + i + k] + 1u);
                out.push_back(' ');
                out += n;
                if (has_uvs) { out.push_back('/'); out += n; }
                else if (has_normals) { out.push_back('/'); }
                if (has_normals) { out.push_back('/'); out += n; }
            }
            out.push_back('\n');
        }
    }
    return out;
}

std::string build_mtl_text(const ExportModel& model, const std::string& base_name,
                           const std::string& map_extension) {
    const bool with_maps = !map_extension.empty();
    // Kd/Ke go white only when the CORRESPONDING map is actually written,
    // because white means "let the map through" to a consumer that multiplies
    // the two — and "fully emissive" to one that does not read map_Ke at all.
    // part_export skips the emissive bake entirely when no material emits, so
    // that second case is the common one and must not render as a white box.
    const bool albedo_map =
        with_maps && !model.maps[static_cast<uint32_t>(MapKind::Albedo)].empty();
    const bool emissive_map =
        with_maps && !model.maps[static_cast<uint32_t>(MapKind::Emissive)].empty();
    std::string out;
    out.reserve(model.materials.size() * 512u + 512u);

    out += "# Wavefront MTL exported by MatterEngine3\n";
    out += "# part: " + model.name + "\n";
    out += "# resolved_hash: " + format_hex64(model.resolved_hash) + "\n";
    out += "# colour keys (Kd, Ke) are sRGB-encoded; Pr/Pm/map_Ka are linear\n";
    if (albedo_map)
        out += "# Kd is white because map_Kd carries the colour; see the "
               "base_albedo comment on each material\n";

    for (const ExportMaterial& material : model.materials) {
        out += "\nnewmtl " + material.name + "\n";
        out += "# engine_material_id: " +
               (material.id == UINT32_MAX ? std::string("none")
                                          : std::to_string(material.id)) +
               "\n";
        const float base_albedo[3] = {srgb_float(material.albedo[0]),
                                      srgb_float(material.albedo[1]),
                                      srgb_float(material.albedo[2])};
        append_vec3(out, "# base_albedo:", base_albedo, kScalarDecimals);

        const float white[3] = {1.0f, 1.0f, 1.0f};
        append_vec3(out, "Kd", albedo_map ? white : base_albedo, kScalarDecimals);
        const float black[3] = {0.0f, 0.0f, 0.0f};
        append_vec3(out, "Ks", black, kScalarDecimals);
        const float emissive[3] = {srgb_float(material.emissive[0]),
                                   srgb_float(material.emissive[1]),
                                   srgb_float(material.emissive[2])};
        append_vec3(out, "Ke", emissive_map ? white : emissive, kScalarDecimals);
        out += "Ns " + format_fixed(specular_exponent(material.roughness), kScalarDecimals) + "\n";
        out += "Ni " + format_fixed(material.ior, kScalarDecimals) + "\n";
        out += "d " + format_fixed(material.opacity, kScalarDecimals) + "\n";
        out += "illum 2\n";
        out += "Pr " + format_fixed(material.roughness, kScalarDecimals) + "\n";
        out += "Pm " + format_fixed(material.metallic, kScalarDecimals) + "\n";
        if (material.alpha_tested)
            out += "# alpha_tested: cutoff " +
                   format_fixed(material.alpha_cutoff, kScalarDecimals) + "\n";
        if (material.double_sided) out += "# double_sided: 1\n";

        if (!with_maps) continue;
        // The whole part shares one atlas, so every material references the
        // same six files; the maps vary across the surface, the material keys
        // do not.
        if (!model.maps[static_cast<uint32_t>(MapKind::Albedo)].empty())
            out += "map_Kd " + map_file_name(base_name, MapKind::Albedo, map_extension) + "\n";
        if (!model.maps[static_cast<uint32_t>(MapKind::Occlusion)].empty())
            out += "map_Ka " + map_file_name(base_name, MapKind::Occlusion, map_extension) + "\n";
        if (!model.maps[static_cast<uint32_t>(MapKind::Normal)].empty()) {
            const std::string normal = map_file_name(base_name, MapKind::Normal, map_extension);
            out += "norm " + normal + "\n";
            out += "map_Bump " + normal + "\n";
        }
        if (!model.maps[static_cast<uint32_t>(MapKind::Roughness)].empty())
            out += "map_Pr " + map_file_name(base_name, MapKind::Roughness, map_extension) + "\n";
        if (!model.maps[static_cast<uint32_t>(MapKind::Metallic)].empty())
            out += "map_Pm " + map_file_name(base_name, MapKind::Metallic, map_extension) + "\n";
        if (!model.maps[static_cast<uint32_t>(MapKind::Emissive)].empty())
            out += "map_Ke " + map_file_name(base_name, MapKind::Emissive, map_extension) + "\n";
    }
    return out;
}

bool write_obj(const ExportModel& model, const ObjWriteOptions& options,
               ObjWriteResult& result, std::string& error) {
    error.clear();
    result = ObjWriteResult{};

    const std::string base = options.base_name.empty() ? model.name : options.base_name;
    if (base.empty()) {
        error = "export needs a base name";
        return false;
    }
    const std::string dir =
        options.directory.empty()
            ? std::string()
            : (options.directory.back() == '/' || options.directory.back() == '\\'
                   ? options.directory
                   : options.directory + "/");
    const std::string extension = texture_format_extension(options.texture_format);

    auto record = [&](const std::string& name, const std::string& bytes) {
        WrittenFile file;
        file.name = name;
        file.bytes = bytes.size();
        file.content_hash = fnv1a64_bytes(bytes.data(), bytes.size());
        result.files.push_back(file);
    };

    const std::string obj_text = build_obj_text(model, base);
    result.obj_name = base + ".obj";
    if (!write_file_text(dir + result.obj_name, obj_text, error)) return false;
    record(result.obj_name, obj_text);

    const std::string mtl_text = build_mtl_text(model, base, extension);
    result.mtl_name = base + ".mtl";
    if (!write_file_text(dir + result.mtl_name, mtl_text, error)) return false;
    record(result.mtl_name, mtl_text);

    if (options.texture_format == TextureFormat::None) return true;

    for (uint32_t m = 0; m < kMapCount; ++m) {
        const ExportImage& image = model.maps[m];
        if (image.empty()) continue;
        const MapKind kind = static_cast<MapKind>(m);
        const std::string name = map_file_name(base, kind, extension);
        if (options.texture_format == TextureFormat::Png) {
            std::vector<uint8_t> encoded;
            if (!encode_png(image, encoded, error)) return false;
            if (!write_file_bytes(dir + name, encoded.data(), encoded.size(), error))
                return false;
            WrittenFile file;
            file.name = name;
            file.bytes = encoded.size();
            file.content_hash = fnv1a64_bytes(encoded.data(), encoded.size());
            result.files.push_back(file);
        } else {
            if (!write_image_file(image, options.texture_format, dir + name, error))
                return false;
        }
    }
    return true;
}

} // namespace matter_export
