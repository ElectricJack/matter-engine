#pragma once

// vt_surface_tape.h — P2 (texel-rate tape): the CPU half of the GPU tape
// interpreter (weight-seam mode 3).
//
// ONE place owns three things that must never drift apart:
//   1. the FIELD-LANE SCAN — which field-derived tape inputs get which vertex
//      lane index (producer computes lane VALUES per vertex; the compositor's
//      packer rewrites the matching ops to LaneRead with the same index —
//      both call vt_scan_surface_lanes over the same canonical text, so the
//      indices agree by construction, not by convention);
//   2. the per-vertex f16 lane packing (vt_compute_surface_lanes /
//      vt_f32_to_f16 — mirrored by unpackHalf2x16 in the shader);
//   3. the GpuSurfOp instruction encoding (struct + kind numbering), mirrored
//      by shaders_vk/vt_surface_tape.glsl. Keep the two in lockstep;
//   4. (P3) the appearance-lane register packing — which tape register each
//      output directive reads, with kVtNoAppearanceReg for "absent". The
//      compositor forwards these on the GPU fill request and the shader's
//      vt_apply_appearance reads them back.
//
// Header-only on purpose (same rationale as vt_chart_gpu.h): the standalone
// compositor test exe must see these without a new engine TU. The only .cpp
// dependency is terrain_field.cpp (SurfaceProgram + the surface_op_fbm*
// accessors used to pre-resolve world ops for non-anchored parts).

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "terrain_field.h"

namespace vt {

// ---------------------------------------------------------------------------
// f16 (IEEE binary16) conversion — round-to-nearest-even, matching the GPU's
// unpackHalf2x16 on the way back. Lanes are smooth field quantities (spec
// §4.3), so half precision is the contract.
// ---------------------------------------------------------------------------
inline uint16_t vt_f32_to_f16(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t sign = (bits >> 16) & 0x8000u;
    uint32_t exp = (bits >> 23) & 0xFFu;
    uint32_t man = bits & 0x7FFFFFu;
    if (exp == 0xFFu) {  // inf/nan
        return static_cast<uint16_t>(sign | 0x7C00u | (man ? 0x200u : 0u));
    }
    // Re-bias 127 -> 15.
    int32_t e = static_cast<int32_t>(exp) - 127 + 15;
    if (e >= 31) return static_cast<uint16_t>(sign | 0x7C00u);   // overflow -> inf
    if (e <= 0) {
        // Subnormal (or zero): shift mantissa (with implicit 1) right.
        if (e < -10) return static_cast<uint16_t>(sign);
        man |= 0x800000u;
        const uint32_t shift = static_cast<uint32_t>(14 - e);
        uint32_t half_man = man >> shift;
        // Round to nearest even.
        const uint32_t rem = man & ((1u << shift) - 1u);
        const uint32_t halfway = 1u << (shift - 1u);
        if (rem > halfway || (rem == halfway && (half_man & 1u)))
            ++half_man;
        return static_cast<uint16_t>(sign | half_man);
    }
    uint32_t half = sign | (static_cast<uint32_t>(e) << 10) | (man >> 13);
    const uint32_t rem = man & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (half & 1u))) ++half;  // RNE
    return static_cast<uint16_t>(half);
}

inline float vt_f16_to_f32(uint16_t half) {
    const uint32_t sign = static_cast<uint32_t>(half & 0x8000u) << 16;
    uint32_t exp = (half >> 10) & 0x1Fu;
    uint32_t man = half & 0x3FFu;
    uint32_t bits;
    if (exp == 0) {
        if (man == 0) {
            bits = sign;
        } else {
            // Subnormal: normalize.
            int32_t e = -1;
            do { ++e; man <<= 1; } while ((man & 0x400u) == 0);
            man &= 0x3FFu;
            bits = sign | static_cast<uint32_t>(113 - e) << 23 | (man << 13);
        }
    } else if (exp == 0x1Fu) {
        bits = sign | 0x7F800000u | (man << 13);
    } else {
        bits = sign | ((exp + 112u) << 23) | (man << 13);
    }
    float out;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

// ---------------------------------------------------------------------------
// Field-lane scan (spec §4.3). Field-derived tape inputs stay at vertex rate:
// each DISTINCT field-derived input, and each `curv` op with a DISTINCT
// radius, is assigned a lane index in first-appearance op order. Cap 8; a
// tape needing more fails mode-3 promotion (fail-soft to mode 2, warn-once).
// ---------------------------------------------------------------------------
constexpr uint32_t kVtMaxSurfaceLanes = 8;

struct VtSurfaceLane {
    int input_code = -1;      // SurfaceInput code, or -1 for a curv lane
    float curv_radius = 0.0f; // meaningful when input_code == -1
};

struct VtSurfaceLaneScan {
    uint32_t count = 0;
    VtSurfaceLane lanes[kVtMaxSurfaceLanes]{};
    bool overflow = false;
    // Diagnostics for the warn-once: the input that overflowed.
    int overflow_code = -1;
    float overflow_radius = 0.0f;
};

inline bool vt_surface_input_is_field(int code) {
    return code == terrain_field::kSurfInHeight ||
           code == terrain_field::kSurfInMoisture ||
           code == terrain_field::kSurfInRelief ||
           code == terrain_field::kSurfInBiome ||
           code == terrain_field::kSurfInFieldSlope;
}

inline VtSurfaceLaneScan vt_scan_surface_lanes(
    const terrain_field::SurfaceProgram& prog) {
    using terrain_field::Op;
    VtSurfaceLaneScan scan;
    auto find_or_add = [&](int code, float radius) -> int {
        for (uint32_t i = 0; i < scan.count; ++i) {
            const VtSurfaceLane& l = scan.lanes[i];
            if (l.input_code != code) continue;
            if (code == -1) {
                uint32_t a = 0, b = 0;
                std::memcpy(&a, &l.curv_radius, sizeof(a));
                std::memcpy(&b, &radius, sizeof(b));
                if (a != b) continue;   // distinct radius = distinct lane
            }
            return static_cast<int>(i);
        }
        if (scan.count >= kVtMaxSurfaceLanes) {
            scan.overflow = true;
            scan.overflow_code = code;
            scan.overflow_radius = radius;
            return -1;
        }
        scan.lanes[scan.count].input_code = code;
        scan.lanes[scan.count].curv_radius = radius;
        return static_cast<int>(scan.count++);
    };
    for (const Op& op : prog.ops) {
        if (op.kind == Op::Input && vt_surface_input_is_field(op.oct))
            find_or_add(op.oct, 0.0f);
        else if (op.kind == Op::FieldCurv)
            find_or_add(-1, op.f0);
        if (scan.overflow) break;
    }
    return scan;
}

// Per-vertex lane values (vertex_count * scan.count f16, lane-major per
// vertex) — computed EXACTLY where classify_vertices' inputs come from: the
// vertex position pushed through the variant's local_to_world (row-major 4x4,
// null = identity), then the field queries. A null `field` yields the same
// deterministic fallback constants SurfaceRuntime::weights_at documents
// (height 0, moisture/relief 0.5, biome 1, fslope 0, curv 0).
inline void vt_compute_surface_lanes(const VtSurfaceLaneScan& scan,
                                     const float* positions,
                                     uint32_t vertex_count,
                                     const terrain_field::FieldRuntime* field,
                                     const float* local_to_world,
                                     std::vector<uint16_t>& out) {
    out.assign(static_cast<size_t>(vertex_count) * scan.count, 0);
    if (scan.count == 0) return;
    for (uint32_t v = 0; v < vertex_count; ++v) {
        const float* p = positions + static_cast<size_t>(v) * 3u;
        float wx = p[0], wz = p[2];
        if (local_to_world != nullptr) {
            const float* m = local_to_world;   // row-major
            wx = m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3];
            wz = m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11];
        }
        uint16_t* dst = out.data() + static_cast<size_t>(v) * scan.count;
        for (uint32_t l = 0; l < scan.count; ++l) {
            const VtSurfaceLane& lane = scan.lanes[l];
            float value = 0.0f;
            if (lane.input_code == -1) {
                value = field ? field->curvature_at(wx, wz, lane.curv_radius)
                              : 0.0f;
            } else {
                switch (lane.input_code) {
                    case terrain_field::kSurfInHeight:
                        value = field ? field->height_at(wx, wz) : 0.0f;
                        break;
                    case terrain_field::kSurfInMoisture:
                        value = field ? field->moisture_at(wx, wz) : 0.5f;
                        break;
                    case terrain_field::kSurfInRelief:
                        value = field ? field->relief_at(wx, wz) : 0.5f;
                        break;
                    case terrain_field::kSurfInBiome:
                        value = field
                                    ? static_cast<float>(field->biome_at(wx, wz))
                                    : 1.0f;
                        break;
                    case terrain_field::kSurfInFieldSlope:
                        value = field ? field->slope_at(wx, wz) : 0.0f;
                        break;
                    default:
                        value = 0.0f;
                        break;
                }
            }
            dst[l] = vt_f32_to_f16(value);
        }
    }
}

// ---------------------------------------------------------------------------
// GpuSurfOp — one packed tape instruction (std430, 48 B). GLSL mirror in
// shaders_vk/vt_surface_tape.glsl; keep the two in lockstep.
//
// LAYOUT (deviation from the spec §4.2 sketch, documented): the sketch's
// third warp float (`wf2`, the declared spare) is repurposed as the EXPLICIT
// warp seed token `wseed` — P1's parser landed the tail as [wseed wfreq wamp]
// with its own seed rather than deriving it from `seed`, so the packed op
// carries it verbatim. Warp PRESENCE rides bit 31 of kind_oct
// (kind | oct << 16 | warp << 31); oct is masked to 15 bits.
//
// Round-trip invariant: every SurfaceProgram op maps 1:1 onto one GpuSurfOp
// (after the lane rewrite / world pre-resolve below) with no information
// loss — asserted by the mode-3 CPU/GPU cross-check tests.
// ---------------------------------------------------------------------------
struct VtGpuSurfOp {
    uint32_t kind_oct = 0;         // kind | (oct << 16) | (warp ? 1u<<31 : 0)
    int32_t a = -1, b = -1, c = -1;  // register operands; LaneRead: a = lane
    float f0 = 0, f1 = 0, f2 = 0, f3 = 0;  // value/freq/gain/lac/edges
    float wf0 = 0, wf1 = 0;        // warp freq / warp amp
    uint32_t wseed = 0;            // explicit warp seed (the wf2 spare slot)
    uint32_t seed = 0;
};
static_assert(sizeof(VtGpuSurfOp) == 48, "VtGpuSurfOp must be 48 B (std430)");

// GPU kind numbering — INDEPENDENT of terrain_field::Op::Kind enum order so a
// CPU enum reorder can never silently re-map shader behaviour. Mirrored by
// the VT_SOP_* defines in vt_surface_tape.glsl.
enum VtSurfOpKind : uint32_t {
    kVtSopConst = 0,
    kVtSopInput = 1,        // oct = SurfaceInput code (local + world position
                            // codes only; field codes become LaneRead)
    kVtSopNoise2 = 2,       // part-local (x, z) fbm
    kVtSopRidge2 = 3,
    kVtSopNoise2World = 4,  // world (x, z) fbm
    kVtSopRidge2World = 5,
    kVtSopNoise3 = 6,       // part-local (x, y, z) fbm, optional warp
    kVtSopRidge3 = 7,
    kVtSopNoise3World = 8,  // world (x, y, z) fbm, optional warp
    kVtSopRidge3World = 9,
    kVtSopAdd = 10,
    kVtSopSub = 11,
    kVtSopMul = 12,
    kVtSopMin = 13,
    kVtSopMax = 14,
    kVtSopClamp = 15,
    kVtSopBlend = 16,
    kVtSopSmoothstep = 17,
    kVtSopAbs = 18,
    kVtSopOneMinus = 19,
    kVtSopPow = 20,
    kVtSopFract = 21,
    kVtSopLaneRead = 22,    // a = lane index (CPU-rewritten Input/FieldCurv)
};

// Absent-directive sentinel for the packed P3 appearance registers. Registers
// are 0..kMaxSurfaceOps-1 (< 0xFF), so 0xFF can never be a real index; the GPU
// request carries the same byte and the shader tests against the same value
// (VT_APP_NO_REG in vt_surface_tape.glsl).
constexpr uint8_t kVtNoAppearanceReg = 0xFFu;

// One packed program, ready for the compositor's tape arena.
struct VtSurfaceTapePack {
    std::vector<VtGpuSurfOp> ops;
    uint8_t weight_reg[terrain_field::kMaxSurfaceMaterials]{};  // per column
    uint32_t weight_reg_count = 0;
    VtSurfaceLaneScan scan;   // lane table (count == the part's lane budget)
    // P3 appearance lanes: the tape register each directive reads, or
    // kVtNoAppearanceReg. Registers survive the pack unchanged — every op maps
    // 1:1 onto one GpuSurfOp (including the world pre-resolve, which rewrites
    // an op's KIND but never its index), so a SurfaceProgram register index is
    // also a GPU register index.
    uint8_t tint_reg[3] = {kVtNoAppearanceReg, kVtNoAppearanceReg,
                           kVtNoAppearanceReg};
    uint8_t rough_bias_reg = kVtNoAppearanceReg;
    uint8_t wetness_reg = kVtNoAppearanceReg;
    uint8_t metallic_reg = kVtNoAppearanceReg;
    bool ok = false;
    bool lane_overflow = false;
    std::string err;
};

// Packs a parsed SurfaceProgram into the flat GPU instruction stream.
//
//   * Field-derived Input ops and FieldCurv ops on WORLD-ANCHORED parts are
//     rewritten to LaneRead (a = lane index from vt_scan_surface_lanes).
//   * On NON-anchored parts every world-dependent op is PRE-RESOLVED to a
//     Const carrying the exact CPU fallback value (world position inputs 0,
//     height 0, moisture/relief 0.5, biome 1, fslope 0, curv 0, and world
//     noise ops evaluated at world origin via surface_op_fbm*). The shader
//     therefore never executes a world op for such parts — one source of
//     truth for the fallback convention (SurfaceRuntime::weights_at), zero
//     shader complexity.
//   * Everything else maps 1:1.
//
// Fails (ok = false) on lane overflow (lane_overflow = true; the part stays
// mode 2) or an internal inconsistency; never throws.
inline bool vt_pack_surface_tape(const terrain_field::SurfaceProgram& prog,
                                 bool world_anchored,
                                 VtSurfaceTapePack& out) {
    using terrain_field::Op;
    out = VtSurfaceTapePack{};
    if (prog.ops.empty() || prog.materials.empty()) {
        out.err = "empty tape";
        return false;
    }
    if (prog.ops.size() >
        static_cast<size_t>(terrain_field::kMaxSurfaceOps)) {  // parse enforces
        out.err = "tape exceeds 64 ops";
        return false;
    }
    if (world_anchored) {
        out.scan = vt_scan_surface_lanes(prog);
        if (out.scan.overflow) {
            out.lane_overflow = true;
            out.err = "field-lane cap (8) exceeded";
            return false;
        }
    }
    // Lane index resolution against the completed scan (matches find_or_add
    // order exactly: the scan already visited the ops in this same order).
    auto lane_of = [&](int code, float radius) -> int {
        for (uint32_t i = 0; i < out.scan.count; ++i) {
            const VtSurfaceLane& l = out.scan.lanes[i];
            if (l.input_code != code) continue;
            if (code == -1) {
                uint32_t a = 0, b = 0;
                std::memcpy(&a, &l.curv_radius, sizeof(a));
                std::memcpy(&b, &radius, sizeof(b));
                if (a != b) continue;
            }
            return static_cast<int>(i);
        }
        return -1;
    };
    auto emit_const = [](VtGpuSurfOp& g, float value) {
        g.kind_oct = kVtSopConst;
        g.f0 = value;
    };

    out.ops.reserve(prog.ops.size());
    for (const Op& op : prog.ops) {
        VtGpuSurfOp g{};
        g.a = op.a;
        g.b = op.b;
        g.c = op.c;
        g.f0 = op.f0;
        g.f1 = op.f1;
        g.f2 = op.f2;
        g.f3 = op.f3;
        g.seed = op.seed;
        const uint32_t oct_bits =
            (static_cast<uint32_t>(op.oct) & 0x7FFFu) << 16;
        const uint32_t warp_bit = op.warp ? (1u << 31) : 0u;
        if (op.warp) {
            g.wf0 = op.wf0;
            g.wf1 = op.wf1;
            g.wseed = op.wseed;
        }
        switch (op.kind) {
            case Op::Const:
                g.kind_oct = kVtSopConst;
                break;
            case Op::Input: {
                const int code = op.oct;
                if (vt_surface_input_is_field(code)) {
                    if (world_anchored) {
                        const int lane = lane_of(code, 0.0f);
                        if (lane < 0) { out.err = "lane map miss"; return false; }
                        g.kind_oct = kVtSopLaneRead;
                        g.a = lane;
                    } else {
                        // CPU fallback constants (weights_at, world == null).
                        float value = 0.0f;
                        if (code == terrain_field::kSurfInMoisture ||
                            code == terrain_field::kSurfInRelief)
                            value = 0.5f;
                        else if (code == terrain_field::kSurfInBiome)
                            value = 1.0f;
                        emit_const(g, value);
                    }
                } else if (code >= terrain_field::kSurfaceInputWorldFirst &&
                           !world_anchored) {
                    emit_const(g, 0.0f);   // wx/wy/wz fallback
                } else {
                    g.kind_oct = kVtSopInput | oct_bits;
                }
                break;
            }
            case Op::FieldCurv:
                if (world_anchored) {
                    const int lane = lane_of(-1, op.f0);
                    if (lane < 0) { out.err = "curv lane map miss"; return false; }
                    g.kind_oct = kVtSopLaneRead;
                    g.a = lane;
                } else {
                    emit_const(g, 0.0f);
                }
                break;
            case Op::Noise2:
                g.kind_oct = kVtSopNoise2 | oct_bits;
                break;
            case Op::Ridge2:
                g.kind_oct = kVtSopRidge2 | oct_bits;
                break;
            case Op::Noise2World:
            case Op::Ridge2World: {
                const bool ridged = op.kind == Op::Ridge2World;
                if (!world_anchored) {
                    emit_const(g, terrain_field::surface_op_fbm2(op, 0.0f,
                                                                 0.0f, ridged));
                } else {
                    g.kind_oct = (ridged ? kVtSopRidge2World
                                         : kVtSopNoise2World) | oct_bits;
                }
                break;
            }
            case Op::Noise3:
                g.kind_oct = kVtSopNoise3 | oct_bits | warp_bit;
                break;
            case Op::Ridge3:
                g.kind_oct = kVtSopRidge3 | oct_bits | warp_bit;
                break;
            case Op::Noise3World:
            case Op::Ridge3World: {
                const bool ridged = op.kind == Op::Ridge3World;
                if (!world_anchored) {
                    emit_const(g, terrain_field::surface_op_fbm3(
                                      op, 0.0f, 0.0f, 0.0f, ridged));
                } else {
                    g.kind_oct = (ridged ? kVtSopRidge3World
                                         : kVtSopNoise3World) |
                                 oct_bits | warp_bit;
                }
                break;
            }
            case Op::Fract:      g.kind_oct = kVtSopFract; break;
            case Op::Add:        g.kind_oct = kVtSopAdd; break;
            case Op::Sub:        g.kind_oct = kVtSopSub; break;
            case Op::Mul:        g.kind_oct = kVtSopMul; break;
            case Op::Min:        g.kind_oct = kVtSopMin; break;
            case Op::Max:        g.kind_oct = kVtSopMax; break;
            case Op::Clamp:      g.kind_oct = kVtSopClamp; break;
            case Op::Blend:      g.kind_oct = kVtSopBlend; break;
            case Op::Smoothstep: g.kind_oct = kVtSopSmoothstep; break;
            case Op::Abs:        g.kind_oct = kVtSopAbs; break;
            case Op::OneMinus:   g.kind_oct = kVtSopOneMinus; break;
            case Op::Pow:        g.kind_oct = kVtSopPow; break;
            case Op::Warp2:
                // Not part of the surface op set (parse rejects it).
                out.err = "warp2 in surface tape";
                return false;
        }
        out.ops.push_back(g);
    }

    // Weight registers per declared material column.
    if (prog.materials.size() > terrain_field::kMaxSurfaceMaterials) {
        out.err = "too many declared materials";
        return false;
    }
    out.weight_reg_count = static_cast<uint32_t>(prog.materials.size());
    for (uint32_t k = 0; k < out.weight_reg_count; ++k) {
        const int reg = prog.materials[k].reg;
        if (reg < 0 || reg >= static_cast<int>(out.ops.size())) {
            out.err = "material weight register out of range";
            return false;
        }
        out.weight_reg[k] = static_cast<uint8_t>(reg);
    }

    // P3 appearance directive registers (absent -> sentinel).
    auto pack_app_reg = [&](int reg, uint8_t& dst) -> bool {
        if (reg < 0) { dst = kVtNoAppearanceReg; return true; }
        if (reg >= static_cast<int>(out.ops.size())) {
            out.err = "appearance directive register out of range";
            return false;
        }
        dst = static_cast<uint8_t>(reg);
        return true;
    };
    for (int c = 0; c < 3; ++c)
        if (!pack_app_reg(prog.tint_reg[c], out.tint_reg[c])) return false;
    if (!pack_app_reg(prog.rough_bias_reg, out.rough_bias_reg)) return false;
    if (!pack_app_reg(prog.wetness_reg, out.wetness_reg)) return false;
    if (!pack_app_reg(prog.metallic_reg, out.metallic_reg)) return false;

    out.ok = true;
    return true;
}

}  // namespace vt
