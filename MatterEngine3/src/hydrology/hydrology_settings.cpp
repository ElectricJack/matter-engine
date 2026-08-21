#include "hydrology_settings.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace hydrology {
namespace {

constexpr std::uint32_t kSchemaVersion = 1;
constexpr std::uint32_t kSolverContractVersion = 1;
constexpr std::uint32_t kShaderContractVersion = 1;
constexpr std::uint32_t kMaxDimension = 4096;
constexpr std::uint64_t kMaxCells = 16ull * 1024ull * 1024ull;

bool finite(float value) { return std::isfinite(value); }
bool finite(const matter::Float2& value) { return finite(value.x) && finite(value.y); }
bool finite(const matter::Float3& value) {
    return finite(value.x) && finite(value.y) && finite(value.z);
}

bool fail(std::string& error, const char* message) {
    error = message;
    return false;
}

bool finite_and_in_range(const matter::HydrologyWorldSettings& authored,
                         std::string& error) {
    if (!finite(authored.domain.origin_m))
        return fail(error, "hydrology origin must be finite");
    if (std::fabs(authored.domain.origin_m.x) > 100000.0f ||
        std::fabs(authored.domain.origin_m.y) > 100000.0f ||
        std::fabs(authored.domain.origin_m.z) > 100000.0f)
        return fail(error, "hydrology origin lies outside the fixed-domain bounds");
    if (authored.domain.nx == 0 || authored.domain.ny == 0 ||
        authored.domain.nz == 0 || authored.domain.nx > kMaxDimension ||
        authored.domain.ny > kMaxDimension || authored.domain.nz > kMaxDimension)
        return fail(error, "hydrology dimensions lie outside the fixed-domain bounds");
    const std::uint64_t cells = static_cast<std::uint64_t>(authored.domain.nx) *
                                authored.domain.ny * authored.domain.nz;
    if (cells > kMaxCells)
        return fail(error, "hydrology domain exceeds the fixed cell budget");
    if (!finite(authored.domain.cell_size_m) || authored.domain.cell_size_m < 0.05f ||
        authored.domain.cell_size_m > 2.0f)
        return fail(error, "hydrology cell size lies outside the fixed-domain bounds");
    if (!finite(authored.dt_s) || authored.dt_s < 0.0001f || authored.dt_s > 0.05f)
        return fail(error, "hydrology time step lies outside the fixed-domain bounds");
    if (!finite(authored.gravity_mps2) || authored.gravity_mps2 < 0.1f ||
        authored.gravity_mps2 > 50.0f)
        return fail(error, "hydrology gravity lies outside the fixed-domain bounds");
    if (!finite(authored.downstream_xz) || authored.downstream_xz.x != 1.0f ||
        authored.downstream_xz.y != 0.0f)
        return fail(error, "hydrology downstream must be exactly (1, 0)");
    if (!finite(authored.residual_head_gradient_xz) ||
        authored.residual_head_gradient_xz.x >= 0.0f ||
        authored.residual_head_gradient_xz.y != 0.0f ||
        authored.residual_head_gradient_xz.x < -1.0f)
        return fail(error, "hydrology residual grade must descend along downstream");
    if (!finite(authored.inlet_flow_m3s) || authored.inlet_flow_m3s < 0.0f ||
        authored.inlet_flow_m3s > 100.0f || !finite(authored.inlet_head_m) ||
        !finite(authored.outlet_head_m) || std::fabs(authored.inlet_head_m) > 10000.0f ||
        std::fabs(authored.outlet_head_m) > 10000.0f)
        return fail(error, "hydrology inlet or outlet values lie outside the fixed-domain bounds");
    if (authored.batch_steps == 0 || authored.batch_steps > 4096 ||
        authored.max_steps == 0 || authored.max_steps > 1048576 ||
        authored.max_steps % authored.batch_steps != 0)
        return fail(error, "hydrology horizon must be a positive whole number of batches");
    return true;
}

void append_u32_le(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift != 32; shift += 8)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}
void append_u64_le(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
    for (unsigned shift = 0; shift != 64; shift += 8)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}
void append_float_le(std::vector<std::uint8_t>& bytes, float value) {
    if (value == 0.0f) value = 0.0f;  // canonicalize negative zero.
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    append_u32_le(bytes, bits);
}

HydrologyKey hash_bytes(const std::vector<std::uint8_t>& bytes) {
    constexpr std::uint64_t kOffset[4] = {
        1469598103934665603ull, 1099511628211ull,
        0x9e3779b185ebca87ull, 0xc2b2ae3d27d4eb4full,
    };
    constexpr std::uint64_t kPrime[4] = {
        1099511628211ull, 0x100000001b3ull,
        0x9ddfea08eb382d69ull, 0x94d049bb133111ebull,
    };
    std::uint64_t state[4] = {kOffset[0], kOffset[1], kOffset[2], kOffset[3]};
    for (std::uint8_t byte : bytes)
        for (unsigned i = 0; i != 4; ++i) {
            state[i] ^= byte;
            state[i] *= kPrime[i];
            state[i] ^= state[i] >> 29;
        }
    HydrologyKey key{};
    for (unsigned word = 0; word != 4; ++word)
        for (unsigned byte = 0; byte != 8; ++byte)
            key.bytes[word * 8 + byte] =
                static_cast<std::uint8_t>(state[word] >> (byte * 8));
    return key;
}

HydrologyKey hash_description_bytes(const HydrologyBakeDescription& value) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(96);
    append_u32_le(bytes, value.schema_version);
    append_u32_le(bytes, value.solver_contract_version);
    append_u32_le(bytes, value.shader_contract_version);
    append_float_le(bytes, value.domain.origin_m.x);
    append_float_le(bytes, value.domain.origin_m.y);
    append_float_le(bytes, value.domain.origin_m.z);
    append_u32_le(bytes, value.domain.nx);
    append_u32_le(bytes, value.domain.ny);
    append_u32_le(bytes, value.domain.nz);
    append_float_le(bytes, value.domain.cell_size_m);
    append_float_le(bytes, value.dt_s);
    append_float_le(bytes, value.gravity_mps2);
    append_float_le(bytes, value.downstream_xz.x);
    append_float_le(bytes, value.downstream_xz.y);
    append_float_le(bytes, value.residual_head_gradient_xz.x);
    append_float_le(bytes, value.residual_head_gradient_xz.y);
    append_float_le(bytes, value.inlet_flow_m3s);
    append_float_le(bytes, value.inlet_head_m);
    append_float_le(bytes, value.outlet_head_m);
    append_u32_le(bytes, value.batch_steps);
    append_u32_le(bytes, value.max_steps);
    append_u64_le(bytes, value.terrain_revision);
    return hash_bytes(bytes);
}

} // namespace

bool validate_and_key(const matter::HydrologyWorldSettings& authored,
                      std::uint64_t terrain_revision,
                      HydrologyBakeDescription& out,
                      std::string& error) {
    error.clear();
    if (!finite_and_in_range(authored, error)) return false;
    HydrologyBakeDescription result{};
    result.schema_version = kSchemaVersion;
    result.solver_contract_version = kSolverContractVersion;
    result.shader_contract_version = kShaderContractVersion;
    result.domain.origin_m = authored.domain.origin_m;
    result.domain.nx = authored.domain.nx;
    result.domain.ny = authored.domain.ny;
    result.domain.nz = authored.domain.nz;
    result.domain.cell_size_m = authored.domain.cell_size_m;
    result.dt_s = authored.dt_s;
    result.gravity_mps2 = authored.gravity_mps2;
    result.downstream_xz = authored.downstream_xz;
    result.residual_head_gradient_xz = authored.residual_head_gradient_xz;
    result.inlet_flow_m3s = authored.inlet_flow_m3s;
    result.inlet_head_m = authored.inlet_head_m;
    result.outlet_head_m = authored.outlet_head_m;
    result.batch_steps = authored.batch_steps;
    result.max_steps = authored.max_steps;
    result.terrain_revision = terrain_revision;
    result.semantic_key = hash_description_bytes(result);
    out = result;
    return true;
}

} // namespace hydrology
