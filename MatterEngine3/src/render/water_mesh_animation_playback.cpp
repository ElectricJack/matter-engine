#include "render/water_mesh_animation_playback.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <system_error>

namespace viewer {
namespace {

bool reject(WaterAnimationFallbackReason reason, const std::string& message,
            WaterAnimationFallback& fallback) {
    fallback = {reason, message};
    return false;
}

bool confined_animation_path(const std::string& authored) {
    if (authored.empty()) return false;
    const std::filesystem::path path(authored);
    if (path.is_absolute() || path.has_root_name() ||
        path.has_root_directory() || path.extension() != ".mhwa")
        return false;
    for (const auto& component : path)
        if (component == "." || component == "..") return false;
    const std::string normalized = path.lexically_normal().generic_string();
    return normalized == authored &&
           normalized.rfind("hydrology/animations/", 0u) == 0u;
}

bool reference_matches(
    const hydrology::HydrologyWaterAnimationReference& reference,
    const hydrology::WaterMeshAnimationArtifact& artifact,
    bool handoff) {
    return artifact.identity == reference.id &&
           artifact.semantic_key == reference.semantic_key &&
           artifact.source_primary_payload_digest ==
               reference.source_primary_payload_digest &&
           artifact.source_secondary_payload_digest ==
               reference.source_secondary_payload_digest &&
           artifact.frames.size() == reference.frame_count &&
           artifact.frames_per_second == reference.frames_per_second &&
           artifact.payload_digest == reference.payload_digest &&
           artifact.frames_per_second == 30u &&
           artifact.frames.size() == 30u &&
           artifact.phase_offset_frames == 15u &&
           artifact.duration_seconds == 1.0f &&
           (handoff ? artifact.source_secondary_payload_digest != 0u
                    : artifact.source_secondary_payload_digest == 0u);
}

}  // namespace

std::uint32_t water_animation_frame(double network_seconds) noexcept {
    if (!std::isfinite(network_seconds) || network_seconds < 0.0) return 0u;
    const double frame = std::floor(network_seconds * 30.0 + 1.0e-9);
    const double wrapped = std::fmod(frame, 30.0);
    return static_cast<std::uint32_t>(wrapped < 0.0 ? wrapped + 30.0
                                                    : wrapped);
}

WaterAnimationFrameSelection
WaterMeshAnimationPlayback::make_selection() const {
    WaterAnimationFrameSelection result{};
    result.draws.resize(assets_.size());
    return result;
}

bool WaterMeshAnimationPlayback::select(
    double network_seconds,
    std::uint32_t frame_slot,
    WaterAnimationFrameSelection& selection,
    WaterAnimationFallback& fallback) noexcept {
    fallback = {};
    if (frame_slot >= last_uploaded_frame_.size())
        return reject(WaterAnimationFallbackReason::InvalidFrameSlot,
                      "water animation Vulkan frame slot is invalid",
                      fallback);
    if (selection.draws.size() != assets_.size())
        return reject(WaterAnimationFallbackReason::InvalidSelection,
                      "water animation selection was not preallocated",
                      fallback);
    const std::uint32_t frame_index =
        water_animation_frame(network_seconds);
    for (std::size_t index = 0u; index != assets_.size(); ++index) {
        const auto& asset = assets_[index];
        hydrology::WaterMeshAnimationFrameSpan span{};
        gpu_meshing::Error error{};
        if (!asset.artifact ||
            !hydrology::water_mesh_animation_frame_span(
                *asset.artifact, frame_index, span, error))
            return reject(WaterAnimationFallbackReason::CorruptArtifact,
                          error.message.empty()
                              ? "water animation frame directory is invalid"
                              : error.message,
                          fallback);
        selection.draws[index] = {
            asset.artifact->identity, asset.handoff, frame_index,
            asset.artifact->material,
            asset.artifact->quantization_bounds_m, span};
    }
    selection.frame_index = frame_index;
    selection.upload_required =
        last_uploaded_frame_[frame_slot] !=
        static_cast<std::int32_t>(frame_index);
    last_uploaded_frame_[frame_slot] =
        static_cast<std::int32_t>(frame_index);
    return true;
}

bool WaterMeshAnimationPlayback::decode_frame_for_test(
    std::size_t asset_index,
    std::uint32_t frame_index,
    std::vector<DecodedWaterAnimationVertex>& vertices,
    std::vector<std::uint32_t>& indices,
    WaterAnimationFallback& fallback) const {
    fallback = {};
    if (asset_index >= assets_.size() || !assets_[asset_index].artifact)
        return reject(WaterAnimationFallbackReason::InvalidSelection,
                      "water animation decode asset index is invalid",
                      fallback);
    gpu_meshing::MeshResult mesh{};
    gpu_meshing::Error error{};
    const auto& artifact = *assets_[asset_index].artifact;
    if (!hydrology::decode_water_mesh_animation_frame(
            artifact, frame_index, mesh, error))
        return reject(WaterAnimationFallbackReason::CorruptArtifact,
                      error.message, fallback);
    std::vector<DecodedWaterAnimationVertex> decoded;
    decoded.reserve(mesh.positions.size() / 3u);
    for (std::size_t vertex = 0u;
         vertex != mesh.positions.size() / 3u; ++vertex) {
        decoded.push_back({
            {mesh.positions[vertex * 3u + 0u],
             mesh.positions[vertex * 3u + 1u],
             mesh.positions[vertex * 3u + 2u]},
            {mesh.normals[vertex * 3u + 0u],
             mesh.normals[vertex * 3u + 1u],
             mesh.normals[vertex * 3u + 2u]},
            artifact.material});
    }
    vertices = std::move(decoded);
    indices = std::move(mesh.indices);
    return true;
}

bool activate_water_mesh_animation_playback(
    const hydrology::HydrologyNetworkArtifact& manifest,
    const std::filesystem::path& cache_root,
    std::uint32_t vulkan_frame_slots,
    std::uint64_t cpu_budget_bytes,
    WaterMeshAnimationPlayback& playback,
    WaterAnimationFallback& fallback) {
    fallback = {};
    if (manifest.section_animations.empty() &&
        manifest.handoff_animations.empty())
        return reject(WaterAnimationFallbackReason::AnimationDisabled,
                      "hydrology manifest has no water animation",
                      fallback);
    if (manifest.state != hydrology::HydrologyNetworkState::Ready ||
        cache_root.empty() || vulkan_frame_slots == 0u ||
        cpu_budget_bytes == 0u)
        return reject(WaterAnimationFallbackReason::InvalidConfiguration,
                      "water animation activation input is invalid",
                      fallback);

    WaterMeshAnimationPlayback candidate{};
    const std::size_t asset_count = manifest.section_animations.size() +
        manifest.handoff_animations.size();
    if (asset_count > std::numeric_limits<std::uint32_t>::max())
        return reject(WaterAnimationFallbackReason::InvalidConfiguration,
                      "water animation has too many synchronized draws",
                      fallback);
    candidate.assets_.reserve(asset_count);
    candidate.last_uploaded_frame_.assign(vulkan_frame_slots, -1);
    const auto load = [&](
        const hydrology::HydrologyWaterAnimationReference& reference,
        bool handoff) {
        if (!confined_animation_path(reference.relative_path))
            return reject(WaterAnimationFallbackReason::MismatchedReference,
                          "water animation manifest path is invalid",
                          fallback);
        const auto path = cache_root / reference.relative_path;
        std::error_code filesystem_error;
        const std::uintmax_t bytes =
            std::filesystem::file_size(path, filesystem_error);
        if (filesystem_error)
            return reject(WaterAnimationFallbackReason::MissingArtifact,
                          "water animation artifact is missing",
                          fallback);
        if (bytes == 0u || bytes > (1ull << 30u))
            return reject(WaterAnimationFallbackReason::CorruptArtifact,
                          "water animation artifact size is invalid",
                          fallback);
        if (bytes > cpu_budget_bytes -
                std::min(candidate.compressed_bytes_, cpu_budget_bytes))
            return reject(WaterAnimationFallbackReason::CpuBudgetExceeded,
                          "water animation compressed CPU budget exceeded",
                          fallback);
        auto mutable_artifact =
            std::make_shared<hydrology::WaterMeshAnimationArtifact>();
        gpu_meshing::Error error{};
        if (!hydrology::load_water_mesh_animation_artifact(
                path, *mutable_artifact, error))
            return reject(WaterAnimationFallbackReason::CorruptArtifact,
                          error.message.empty()
                              ? "water animation artifact is corrupt"
                              : error.message,
                          fallback);
        if (!reference_matches(reference, *mutable_artifact, handoff))
            return reject(WaterAnimationFallbackReason::MismatchedReference,
                          "water animation artifact does not match its manifest reference",
                          fallback);
        candidate.compressed_bytes_ += bytes;
        candidate.assets_.push_back({
            reference,
            std::shared_ptr<const hydrology::WaterMeshAnimationArtifact>(
                std::move(mutable_artifact)),
            handoff, bytes});
        return true;
    };
    for (const auto& reference : manifest.section_animations)
        if (!load(reference, false)) return false;
    for (const auto& reference : manifest.handoff_animations)
        if (!load(reference, true)) return false;
    candidate.capacity_.draw_count =
        static_cast<std::uint32_t>(candidate.assets_.size());
    for (std::uint32_t frame = 0u; frame != 30u; ++frame) {
        std::uint64_t vertex_bytes = 0u;
        std::uint64_t vertex_count = 0u;
        std::uint64_t index_bytes = 0u;
        for (const auto& asset : candidate.assets_) {
            if (!asset.artifact || frame >= asset.artifact->frames.size())
                return reject(WaterAnimationFallbackReason::CorruptArtifact,
                              "water animation frame directory is incomplete",
                              fallback);
            const auto& record = asset.artifact->frames[frame];
            const std::uint64_t asset_vertex_bytes =
                static_cast<std::uint64_t>(record.vertex_count) *
                sizeof(hydrology::PackedWaterAnimationVertex);
            const std::uint64_t asset_index_bytes =
                static_cast<std::uint64_t>(record.index_count) *
                sizeof(std::uint32_t);
            if (asset_vertex_bytes >
                    std::numeric_limits<std::uint64_t>::max() - vertex_bytes ||
                record.vertex_count >
                    std::numeric_limits<std::uint64_t>::max() - vertex_count ||
                asset_index_bytes >
                    std::numeric_limits<std::uint64_t>::max() - index_bytes)
                return reject(WaterAnimationFallbackReason::CorruptArtifact,
                              "water animation synchronized frame size overflows",
                              fallback);
            vertex_bytes += asset_vertex_bytes;
            vertex_count += record.vertex_count;
            index_bytes += asset_index_bytes;
        }
        candidate.capacity_.packed_vertex_bytes = std::max(
            candidate.capacity_.packed_vertex_bytes, vertex_bytes);
        candidate.capacity_.decoded_vertex_count = std::max(
            candidate.capacity_.decoded_vertex_count, vertex_count);
        candidate.capacity_.index_bytes = std::max(
            candidate.capacity_.index_bytes, index_bytes);
    }
    if (!candidate.capacity_.valid())
        return reject(WaterAnimationFallbackReason::CorruptArtifact,
                      "water animation contains no renderable synchronized frame",
                      fallback);
    playback = std::move(candidate);
    return true;
}

}  // namespace viewer
