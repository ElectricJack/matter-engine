#pragma once

// Acceptance replay: no PhysX, Vulkan, rebake, or tolerance changes.
// MWCUT001 is emitted by dump_cell_boundary_repro in the production builder.
#include "hydrology/water_mesh_continuity.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace water_boundary_replay {

struct Reader {
    const std::vector<std::uint8_t>& bytes;
    std::size_t offset = 0u;

    void require(std::size_t count) const {
        if (count > bytes.size() - offset)
            throw std::runtime_error("truncated MWCUT001 capture");
    }
    std::uint32_t u32() {
        require(4u);
        std::uint32_t value = 0u;
        for (unsigned shift = 0u; shift != 32u; shift += 8u)
            value |= static_cast<std::uint32_t>(bytes[offset++]) << shift;
        return value;
    }
    std::uint64_t u64() {
        const std::uint64_t low = u32();
        return low | (static_cast<std::uint64_t>(u32()) << 32u);
    }
    float floating() {
        const auto bits = u32();
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    matter::Float3 point() { return {floating(), floating(), floating()}; }
    std::string string() {
        const auto count = u32();
        if (count > 1024u) throw std::runtime_error("oversized capture string");
        require(count);
        std::string value(reinterpret_cast<const char*>(bytes.data() + offset), count);
        offset += count;
        return value;
    }
    template <typename Value, typename Read>
    void elements(std::vector<Value>& output, Read read) {
        const auto count = u64();
        if (count > 64ull * 1024ull * 1024ull ||
            count > (bytes.size() - offset) / 4u)
            throw std::runtime_error("invalid capture mesh element count");
        output.resize(static_cast<std::size_t>(count));
        for (auto& value : output) value = read();
    }
    gpu_meshing::MeshResult mesh() {
        gpu_meshing::MeshResult value;
        elements(value.positions, [&] { return floating(); });
        elements(value.normals, [&] { return floating(); });
        elements(value.indices, [&] { return u32(); });
        value.material = u32();
        value.content_digest = u64();
        return value;
    }
};

struct Capture {
    std::uint32_t frame = 0u;
    std::string boundary;
    hydrology::WaterCellOwnershipCut cut;
    float tolerance = 0.0f;
    gpu_meshing::MeshResult first;
    gpu_meshing::MeshResult second;
    std::vector<std::uint8_t> header;
};

inline Capture load(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) throw std::runtime_error("could not open capture");
    const auto size = stream.tellg();
    if (size < 8 || size > 1024ll * 1024ll * 1024ll)
        throw std::runtime_error("invalid capture size");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!stream || std::memcmp(bytes.data(), "MWCUT001", 8u) != 0)
        throw std::runtime_error("invalid MWCUT001 capture");
    Reader reader{bytes, 8u};
    Capture value;
    value.frame = reader.u32();
    value.boundary = reader.string();
    value.cut.lattice.version = reader.u32();
    value.cut.lattice.origin_m = reader.point();
    value.cut.lattice.voxel_m = reader.floating();
    value.cut.signed_cut_m = reader.floating();
    value.tolerance = reader.floating();
    auto& handoff = value.cut.handoff;
    handoff.id = reader.string();
    handoff.upstream_section_id = reader.string();
    handoff.downstream_section_id = reader.string();
    handoff.lip_origin_m = reader.point();
    handoff.tangent = reader.point();
    handoff.lateral = reader.point();
    handoff.up = reader.point();
    handoff.discharge_m3s = reader.floating();
    handoff.width_m = reader.floating();
    handoff.effective_depth_m = reader.floating();
    handoff.channel_depth_m = reader.floating();
    handoff.channel_asymmetry = reader.floating();
    handoff.initial_speed_mps = reader.floating();
    handoff.overlap_m = reader.floating();
    handoff.upstream_visual_cut_m = reader.floating();
    handoff.downstream_visual_cut_m = reader.floating();
    handoff.temporary_dam_exclusion_bounds_m.minimum = reader.point();
    handoff.temporary_dam_exclusion_bounds_m.maximum = reader.point();
    handoff.semantic_key = reader.u64();
    value.header.assign(bytes.begin(), bytes.begin() + reader.offset);
    value.first = reader.mesh();
    value.second = reader.mesh();
    if (reader.offset != bytes.size())
        throw std::runtime_error("unexpected capture trailing bytes");
    return value;
}

struct Measurement {
    bool measured = false;
    bool weldable = false;
    hydrology::WaterCutContourMetrics metrics;
    hydrology::FluidBakeError error;
    std::string diagnostic;
};

inline Measurement measure(const Capture& value) {
    Measurement result;
    result.measured = hydrology::measure_water_cell_boundary_continuity(
        value.first, value.second, value.cut, value.tolerance,
        result.metrics, result.error, &result.diagnostic);
    result.weldable = result.measured && hydrology::water_cut_is_assertion_weldable(
        result.metrics, value.tolerance);
    return result;
}

inline void print(const Capture& value, const Measurement& result) {
    std::printf("CPU boundary replay: frame=%u boundary=%s measured=%d weldable=%d "
                "tolerance=%.9g hausdorff=%.9g normalDot=%.9g openEdges=%u duplicates=%u "
                "triangles=%zu/%zu\n%s%s\n",
                value.frame, value.boundary.c_str(), result.measured, result.weldable,
                value.tolerance, result.metrics.symmetric_hausdorff_m,
                result.metrics.minimum_normal_dot, result.metrics.unmatched_open_edges,
                result.metrics.duplicate_coplanar_triangles,
                value.first.indices.size() / 3u, value.second.indices.size() / 3u,
                result.diagnostic.c_str(), result.error.message.c_str());
    std::fflush(stdout);
}

inline std::string reason(const Measurement& value) {
    return value.diagnostic.substr(0u, value.diagnostic.find_first_of(" \n"));
}

inline bool same_failure(const Measurement& value, const Measurement& original,
                         float tolerance) {
    // Keep the captured gate-failure combination, not an empty/invalid mesh
    // or a new distance, normal, or duplicate-ownership failure.
    return value.measured && !value.weldable &&
        (value.metrics.unmatched_open_edges != 0u) ==
            (original.metrics.unmatched_open_edges != 0u) &&
        (value.metrics.symmetric_hausdorff_m <= tolerance) ==
            (original.metrics.symmetric_hausdorff_m <= tolerance) &&
        (value.metrics.rms_distance_m <= tolerance) ==
            (original.metrics.rms_distance_m <= tolerance) &&
        (value.metrics.minimum_normal_dot >= 0.995f) ==
            (original.metrics.minimum_normal_dot >= 0.995f) &&
        (value.metrics.duplicate_coplanar_triangles == 0u) ==
            (original.metrics.duplicate_coplanar_triangles == 0u) &&
        reason(value) == reason(original);
}

inline void compact(gpu_meshing::MeshResult& mesh) {
    std::vector<std::uint32_t> remap(mesh.positions.size() / 3u, UINT32_MAX);
    std::vector<float> positions, normals;
    for (auto& index : mesh.indices) {
        if (index >= remap.size()) throw std::runtime_error("invalid replay mesh index");
        auto& replacement = remap[index];
        if (replacement == UINT32_MAX) {
            replacement = static_cast<std::uint32_t>(positions.size() / 3u);
            positions.insert(positions.end(), mesh.positions.begin() + index * 3u,
                             mesh.positions.begin() + index * 3u + 3u);
            normals.insert(normals.end(), mesh.normals.begin() + index * 3u,
                           mesh.normals.begin() + index * 3u + 3u);
        }
        index = replacement;
    }
    mesh.positions = std::move(positions);
    mesh.normals = std::move(normals);
    mesh.content_digest = gpu_meshing::mesh_content_digest(mesh);
}

inline void save(const Capture& value, const std::filesystem::path& path) {
    if (std::filesystem::exists(path))
        throw std::runtime_error("refusing to overwrite a capture");
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(value.header.data()),
                 static_cast<std::streamsize>(value.header.size()));
    const auto u32 = [&](std::uint32_t number) {
        for (unsigned shift = 0u; shift != 32u; shift += 8u)
            stream.put(static_cast<char>((number >> shift) & 0xffu));
    };
    const auto u64 = [&](std::uint64_t number) {
        u32(static_cast<std::uint32_t>(number));
        u32(static_cast<std::uint32_t>(number >> 32u));
    };
    const auto floats = [&](const auto& values) {
        u64(values.size());
        for (const float number : values) {
            std::uint32_t bits = 0u;
            std::memcpy(&bits, &number, sizeof(bits));
            u32(bits);
        }
    };
    for (const auto* mesh : {&value.first, &value.second}) {
        floats(mesh->positions);
        floats(mesh->normals);
        u64(mesh->indices.size());
        for (const auto index : mesh->indices) u32(index);
        u32(mesh->material);
        u64(mesh->content_digest);
    }
    stream.flush();
    if (!stream) throw std::runtime_error("could not write minimized capture");
}

inline int run(int argc, char** argv) {
    try {
        const bool minimize = argc >= 2 && std::strcmp(argv[1], "--boundary-minimize") == 0;
        if ((!minimize && (argc != 3 || std::strcmp(argv[1], "--boundary-replay") != 0)) ||
            (minimize && argc != 4 && argc != 5)) {
            std::printf("Usage: --boundary-replay capture.bin OR "
                        "--boundary-minimize capture.bin output.bin [max-probes]\n");
            return 2;
        }
        Capture value = load(argv[2]);
        const Measurement original = measure(value);
        print(value, original);
        if (!minimize) return original.weldable ? 0 : 1;
        if (!same_failure(original, original, value.tolerance))
            throw std::runtime_error("capture is not a measured continuity failure");
        const unsigned long maximum = argc == 5 ? std::stoul(argv[4]) : 256u;
        if (maximum == 0u || maximum > 10000u)
            throw std::runtime_error("max-probes must be in [1,10000]");
        unsigned long probes = 0u;
        bool changed = true;
        while (changed && probes < maximum) {
            changed = false;
            for (auto* mesh : {&value.first, &value.second}) {
                std::size_t block = std::max<std::size_t>(1u, mesh->indices.size() / 6u);
                while (block != 0u && probes < maximum) {
                    for (std::size_t start = 0u;
                         start < mesh->indices.size() / 3u && probes < maximum;) {
                        const auto count = mesh->indices.size() / 3u;
                        const auto end = std::min(count, start + block);
                        if (end - start == count) break;
                        auto previous = std::move(mesh->indices);
                        mesh->indices.assign(previous.begin(), previous.begin() + start * 3u);
                        mesh->indices.insert(mesh->indices.end(), previous.begin() + end * 3u,
                                             previous.end());
                        ++probes;
                        if (same_failure(measure(value), original, value.tolerance)) {
                            changed = true;
                            std::printf("minimize probe=%lu triangles=%zu/%zu\n", probes,
                                        value.first.indices.size() / 3u,
                                        value.second.indices.size() / 3u);
                            std::fflush(stdout);
                        } else {
                            mesh->indices = std::move(previous);
                            start += block;
                        }
                    }
                    if (block == 1u) break;
                    block = std::max<std::size_t>(1u, block / 2u);
                }
            }
        }
        compact(value.first);
        compact(value.second);
        const auto final_result = measure(value);
        if (!same_failure(final_result, original, value.tolerance))
            throw std::runtime_error("compaction changed the captured failure");
        save(value, argv[3]);
        print(value, final_result);
        return 0;
    } catch (const std::exception& error) {
        std::printf("boundary replay error: %s\n", error.what());
        return 2;
    }
}

}  // namespace water_boundary_replay
