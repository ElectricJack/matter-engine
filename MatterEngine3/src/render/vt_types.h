#pragma once

// Chart-space virtual texturing — runtime seam (contract C2).
// Owner of the residency side: vt_residency.{h,cpp} (WP-E).
// Implementations of the filler seam: vt_stub_filler.cpp (WP-E, flat
// material fill) and vt_compositor.{h,cpp} (WP-D, the real tier-1
// compositor + GPU BC encode). The residency layer resolves each request
// (destination slot, chart table) before invoking the filler; the filler
// records GPU work into the provided command buffer and never touches
// indirection state — mapping updates are the residency layer's job after
// the fill is submitted.
//
// C++17: the batch is (pointer, count), not std::span (plan C2 wrote span;
// this header is the binding form).

#include <cstddef>
#include <cstdint>

#include <vulkan/vulkan_core.h>

#include "chart_atlas.h"

namespace vt {

// One page fill, fully resolved by the residency layer.
struct VtFillRequest {
    uint64_t variant_hash = 0;    // resolved_hash of the part variant
    uint16_t rung = 0;            // LOD rung whose chart table applies
    uint16_t mip = 0;             // virtual mip the page belongs to
    uint16_t page_x = 0;          // page coords at `mip`
    uint16_t page_y = 0;
    uint32_t physical_slot = 0;   // destination page slot in the pool
    const chart_atlas::ChartAtlasRung* atlas = nullptr;  // borrowed, non-null
    // Residency-layer handle giving the filler access to the variant's mesh
    // (positions/normals/TriEx material ids) and material bindings. Opaque
    // here; the concrete type is agreed between vt_residency and the filler
    // implementations (extend there, both sides recompile).
    const void* part_context = nullptr;
};

class VtPageFiller {
  public:
    virtual ~VtPageFiller() = default;
    // Record fills for `count` requests into `cmd`. Deterministic given
    // identical inputs (no time/random); must not submit or wait.
    virtual void fill(VkCommandBuffer cmd, const VtFillRequest* batch, size_t count) = 0;
};

}  // namespace vt
