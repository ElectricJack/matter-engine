#pragma once

#include "matter/render_eligibility.h"
#include "part_bundle.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace matter {

struct PartRenderPolicy {
    bool ray_traced = true;
    std::vector<RayTracingOverride> child_overrides;
};

namespace render_policy_detail {

inline constexpr std::uint32_t kMagic = 0x5054524du;  // "MRTP"
inline constexpr std::uint32_t kVersion = 1u;

inline void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (unsigned shift = 0; shift != 32; shift += 8)
        out.push_back(static_cast<std::uint8_t>(value >> shift));
}

inline void put_u64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (unsigned shift = 0; shift != 64; shift += 8)
        out.push_back(static_cast<std::uint8_t>(value >> shift));
}

inline bool get_u32(const std::vector<std::uint8_t>& bytes, std::size_t& at,
                    std::uint32_t& value) {
    if (bytes.size() - std::min(bytes.size(), at) < 4) return false;
    value = 0;
    for (unsigned shift = 0; shift != 32; shift += 8)
        value |= static_cast<std::uint32_t>(bytes[at++]) << shift;
    return true;
}

inline bool get_u64(const std::vector<std::uint8_t>& bytes, std::size_t& at,
                    std::uint64_t& value) {
    if (bytes.size() - std::min(bytes.size(), at) < 8) return false;
    value = 0;
    for (unsigned shift = 0; shift != 64; shift += 8)
        value |= static_cast<std::uint64_t>(bytes[at++]) << shift;
    return true;
}

}  // namespace render_policy_detail

inline bool save_part_render_policy(const std::string& path,
                                    std::uint64_t resolved_hash,
                                    const PartRenderPolicy& policy) {
    if (policy.child_overrides.size() > UINT32_MAX) return false;
    std::vector<std::uint8_t> bytes;
    bytes.reserve(21u + policy.child_overrides.size());
    render_policy_detail::put_u32(bytes, render_policy_detail::kMagic);
    render_policy_detail::put_u32(bytes, render_policy_detail::kVersion);
    render_policy_detail::put_u64(bytes, resolved_hash);
    bytes.push_back(policy.ray_traced ? 1u : 0u);
    render_policy_detail::put_u32(
        bytes, static_cast<std::uint32_t>(policy.child_overrides.size()));
    for (RayTracingOverride override_value : policy.child_overrides) {
        const std::uint8_t encoded = static_cast<std::uint8_t>(override_value);
        if (encoded > static_cast<std::uint8_t>(RayTracingOverride::Enabled))
            return false;
        bytes.push_back(encoded);
    }
    return part_bundle::write_section(path, resolved_hash,
                                      part_bundle::kSectionRenderPolicy,
                                      bytes.data(), bytes.size());
}

inline bool load_part_render_policy(const std::string& path,
                                    std::uint64_t resolved_hash,
                                    std::size_t expected_child_count,
                                    PartRenderPolicy& policy) {
    policy.ray_traced = true;
    policy.child_overrides.assign(expected_child_count,
                                  RayTracingOverride::Inherit);

    const std::vector<std::uint32_t> tags = part_bundle::section_tags(path);
    if (std::find(tags.begin(), tags.end(), part_bundle::kSectionRenderPolicy) ==
        tags.end())
        return true;

    std::vector<std::uint8_t> bytes;
    if (!part_bundle::read_section(path, resolved_hash,
                                   part_bundle::kSectionRenderPolicy, bytes))
        return false;
    std::size_t at = 0;
    std::uint32_t magic = 0, version = 0, child_count = 0;
    std::uint64_t stored_hash = 0;
    if (!render_policy_detail::get_u32(bytes, at, magic) ||
        !render_policy_detail::get_u32(bytes, at, version) ||
        !render_policy_detail::get_u64(bytes, at, stored_hash) ||
        at >= bytes.size())
        return false;
    const std::uint8_t part_ray_traced = bytes[at++];
    if (!render_policy_detail::get_u32(bytes, at, child_count)) return false;
    if (magic != render_policy_detail::kMagic ||
        version != render_policy_detail::kVersion ||
        stored_hash != resolved_hash || part_ray_traced > 1u ||
        child_count != expected_child_count ||
        bytes.size() - at != static_cast<std::size_t>(child_count))
        return false;

    std::vector<RayTracingOverride> overrides;
    overrides.reserve(child_count);
    for (std::uint32_t index = 0; index != child_count; ++index) {
        const std::uint8_t encoded = bytes[at++];
        if (encoded > static_cast<std::uint8_t>(RayTracingOverride::Enabled))
            return false;
        overrides.push_back(static_cast<RayTracingOverride>(encoded));
    }
    policy.ray_traced = part_ray_traced != 0u;
    policy.child_overrides = std::move(overrides);
    return true;
}

}  // namespace matter
