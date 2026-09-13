#include "authored_world_cache.h"
#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>
#include <unordered_set>
#include <utility>
namespace authored_world_cache {
namespace {
constexpr uint32_t kSchema = 1;
constexpr uint32_t kMagic = 0x57444341; // ACDW, little endian
constexpr size_t kMaxBytes = 64u * 1024u * 1024u;
constexpr size_t kMaxString = 16u * 1024u * 1024u;
constexpr size_t kMaxItems = 1000000;

uint64_t checksum(const char* p, size_t n) {
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < n; i++) {
        h ^= static_cast<unsigned char>(p[i]);
        h *= 1099511628211ull;
    }
    return h;
}

#include "authored_world_cache_fields.inc"

template <class A> void fields(A& a, matter::Float3& v) {
    a(v.x, v.y, v.z);
}
template <class A> void fields(A& a, matter::Mat4f& v) {
    a(v.m);
}
template <class A> void fields(A& a, Material& v) {
    a(v.name, v.index, v.definition);
}
template <class A> void fields(A& a, matter::WorldDefinition& v) {
    a(v.roots, v.lights, v.entities, v.materials, v.props, v.settings);
}
template <class A> void fields(A& a, Snapshot& v) {
    a(v.world, v.materials);
}
template <class T> struct IsVector : std::false_type {};
template <class T, class A> struct IsVector<std::vector<T, A>> : std::true_type {};
// Arithmetic is encoded field by field in little-endian order. The decoder
// also budgets aggregate allocations, so nested vectors cannot amplify a small
// malformed payload into unbounded memory use. No native struct bytes are saved.
struct Archive {
    std::string& bytes;
    bool reading = false, ok = true;
    size_t pos = 0, allocated = 0;
    template <class... T> void operator()(T&... values) {
        (value(values), ...);
    }
    template <class T> void value(T& v) {
        if (!ok)
            return;
        if constexpr (std::is_same_v<T, bool>) {
            uint8_t n = v ? 1 : 0;
            value(n);
            if (n > 1)
                ok = false;
            else
                v = n != 0;
        } else if constexpr (std::is_enum_v<T>) {
            int32_t n = static_cast<int32_t>(v);
            value(n);
            v = static_cast<T>(n);
        } else if constexpr (std::is_arithmetic_v<T>) {
            static_assert(sizeof(T) <= 8);
            uint64_t n = 0;
            if (reading) {
                if (sizeof(T) > bytes.size() - pos) {
                    ok = false;
                    return;
                }
                for (size_t i = 0; i < sizeof(T); i++)
                    n |= uint64_t(static_cast<unsigned char>(bytes[pos++])) << (i * 8);
                std::memcpy(&v, &n, sizeof(T));
            } else {
                std::memcpy(&n, &v, sizeof(T));
                for (size_t i = 0; i < sizeof(T); i++)
                    bytes.push_back(char(n >> (i * 8)));
            }
            if constexpr (std::is_floating_point_v<T>)
                if (!std::isfinite(v))
                    ok = false;
        } else if constexpr (std::is_same_v<T, std::string>) {
            uint32_t n = reading ? 0 : static_cast<uint32_t>(v.size());
            if (!reading && v.size() > kMaxString) {
                ok = false;
                return;
            }
            value(n);
            if (!ok || n > kMaxString) {
                ok = false;
                return;
            }
            if (reading) {
                if (n > bytes.size() - pos || n > kMaxBytes - allocated) {
                    ok = false;
                    return;
                }
                allocated += n;
                v.assign(bytes.data() + pos, n);
                pos += n;
            } else
                bytes.append(v);
        } else if constexpr (IsVector<T>::value) {
            uint32_t n = reading ? 0 : static_cast<uint32_t>(v.size());
            if (!reading && v.size() > kMaxItems) {
                ok = false;
                return;
            }
            value(n);
            if (!ok || n > kMaxItems || (reading && n > bytes.size() - pos)) {
                ok = false;
                return;
            }
            if (reading) {
                using Item = typename T::value_type;
                if (n > (kMaxBytes - allocated) / sizeof(Item)) {
                    ok = false;
                    return;
                }
                allocated += size_t(n) * sizeof(Item);
                v.resize(n);
            }
            for (auto& x : v)
                value(x);
        } else if constexpr (std::is_array_v<T>) {
            for (auto& x : v)
                value(x);
        } else
            fields(*this, v);
        if (bytes.size() > kMaxBytes)
            ok = false;
    }
};
// These enums are dense index domains, not physical slice counts or flags.
// Refer to their named endpoints so validation documents the authored contract.
template <class E> bool in_domain(E value, E first, E last) {
    return static_cast<int>(value) >= static_cast<int>(first) &&
           static_cast<int>(value) <= static_cast<int>(last);
}

bool valid(const Snapshot& s) {
    const auto& w = s.world;
    if (w.hydrology || w.river_network || w.terrain_collision ||
        s.materials.size() > size_t(MATERIAL_MAX_TOTAL - MaterialRegistryStaticCount()))
        return false;
    const auto& t = w.settings;
    if (t.sector_size <= 0 || t.y_max <= t.y_min || t.fog.cloud_count < 0 ||
        t.fog.cloud_count > matter::kMaxCloudLayers)
        return false;
    if (!in_domain(t.volumetrics.froxel_xy_scale, matter::FroxelXyScale::X0_5,
                   matter::FroxelXyScale::X2_0) ||
        !in_domain(t.volumetrics.froxel_depth_slices, matter::FroxelDepthSlices::D64,
                   matter::FroxelDepthSlices::D256))
        return false;
    std::unordered_set<std::string> names;
    for (size_t i = 0; i < s.materials.size(); i++) {
        const auto& m = s.materials[i];
        const auto& d = m.definition;
        if (m.index != MaterialRegistryStaticCount() + int(i) || m.name.empty() ||
            !names.insert(m.name).second || d.groundTilesetSlot != -1 ||
            d.groundMacroSlot != -1)
            return false;
        // Material schema: shading is smooth/flat; mesher is marching cubes/
        // oriented cubes. Both selectors have the closed integer domain [0,1].
        if (d.flatShading < 0 || d.flatShading > 1 || d.meshingAlgorithm < 0 ||
            d.meshingAlgorithm > 1)
            return false;
    }
    for (const auto& m : w.materials) {
        const int i = m.index - MaterialRegistryStaticCount();
        if (i < 0 || size_t(i) >= s.materials.size() || m.name != s.materials[i].name ||
            m.detail_density < 0)
            return false;
    }
    for (const auto& r : w.roots)
        if (r.module.empty() ||
            !in_domain(r.fluid_collider.shape, matter::WorldFluidColliderShape::None,
                       matter::WorldFluidColliderShape::Box))
            return false;
    for (const auto& l : w.lights)
        if (!in_domain(l.kind, matter::WorldLightKind::Point,
                       matter::WorldLightKind::Spot))
            return false;
    for (const auto& p : w.props)
        if (!in_domain(p.kind, matter::WorldPropSpec::Kind::Float,
                       matter::WorldPropSpec::Kind::Enum) ||
            (p.has_range && p.min > p.max))
            return false;
    return true;
}
} // namespace
bool capture(const matter::WorldDefinition& world, std::string& blob) {
    blob.clear();
    try {
        Snapshot s;
        s.world = world;
        for (int i = MaterialRegistryStaticCount(); i < MaterialRegistryCount(); i++) {
            Material m;
            m.index = i;
            const char* name = MaterialRegistryNameOf(i);
            if (!name)
                return false;
            m.name = name;
            m.definition = *MaterialRegistryGet(i);
            // Captured before publication: runtime overrides are not authored state.
            if (m.definition.groundTilesetSlot != -1 ||
                m.definition.groundMacroSlot != -1)
                return false;
            s.materials.push_back(std::move(m));
        }
        if (!valid(s))
            return false;
        std::string data;
        Archive a{data};
        uint32_t magic = kMagic, schema = kSchema,
                 materialSchema = MaterialRegistrySchemaVersion();
        a(magic, schema, materialSchema, s);
        if (!a.ok)
            return false;
        uint64_t hash = checksum(data.data(), data.size());
        a(hash);
        if (!a.ok)
            return false;
        blob = std::move(data);
        return true;
    } catch (...) {
        blob.clear();
        return false;
    }
}

bool decode(const std::string& blob, Snapshot& out) {
    try {
        if (blob.size() < 20 || blob.size() > kMaxBytes)
            return false;
        uint64_t stored = 0;
        for (int i = 0; i < 8; i++)
            stored |= uint64_t(static_cast<unsigned char>(blob[blob.size() - 8 + i]))
                      << (8 * i);
        if (checksum(blob.data(), blob.size() - 8) != stored)
            return false;
        std::string data(blob.data(), blob.size() - 8);
        Archive a{data, true};
        uint32_t magic = 0, schema = 0, materialSchema = 0;
        a(magic, schema, materialSchema);
        if (!a.ok || magic != kMagic || schema != kSchema ||
            materialSchema != MaterialRegistrySchemaVersion())
            return false;
        Snapshot s;
        a(s);
        if (!a.ok || a.pos != data.size() || !valid(s))
            return false;
        out = std::move(s);
        return true;
    } catch (...) {
        return false;
    }
}

bool replay_materials(const Snapshot& s) {
    if (!valid(s))
        return false;
    MaterialRegistryResetDynamic();
    for (const auto& m : s.materials)
        if (MaterialRegistryDefineDynamic(&m.definition, m.name.c_str()) != m.index) {
            MaterialRegistryResetDynamic();
            return false;
        }
    return true;
}
} // namespace authored_world_cache
