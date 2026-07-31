#pragma once

// Chart-VT residency budgets — the env-consolidation pilot (property-system
// design S8 / S10 "Phase 2"). Six MATTER_VT_* vars that used to be read through
// a copy-pasted env_u32 helper into function-local statics at
// vt_residency.cpp:306-337 now live in ONE settings struct with ONE schema, so
// the editor's Tunables panel shows them (and their env-forced source) instead
// of them being invisible process-start overrides.
//
// ENGINE/EDITOR SPLIT. The engine cannot depend on the editor's Registry, so:
//   * the engine owns the struct (vt_residency_budgets(), a mutable singleton)
//     and the schema (vt_residency_budgets_group()) right here;
//   * VtResidency reads the struct, never getenv;
//   * ensure_vt_residency_env_applied() applies layer 5 on first use, so a
//     headless/test/tools run that never binds a registry keeps EXACTLY the
//     old env behavior;
//   * the editor binds the same struct into its Registry at startup (User
//     scope: these are GPU-memory budgets, per-machine taste, not project
//     data) and its apply_env pass is idempotent with the engine's.
//
// LIVE vs INIT-TIME. VtResidency::init() sizes the variant record table from
// max_variants and the indirection arena from indirection_mb — both are buffer
// allocations that cannot change without recreating the renderer, which a WORLD
// reload does not do. They are therefore ReadOnly here (displayed with their
// env source, not editable), rather than RequiresReload, which would promise a
// reload that does not in fact re-run init. The other four are re-read by
// VtResidency::begin_frame every frame, so editing them is genuinely live.

#include "matter/props.h"

#include <cstdint>

namespace matter {

struct VtResidencyBudgets {
    // Soft bookkeeping bound sizing the variant record table (64 B per slot);
    // the hard ceiling is 65534 (the 16-bit feedback/draw-record transport).
    uint32_t max_variants = 32768;
    // Feedback-driven page fills recorded per frame.
    uint32_t fills_per_frame = 8;
    // Per-variant tail fills, budgeted SEPARATELY: a streaming burst registers
    // many variants per frame and each renders legacy-flat until its single
    // tail page lands, so tails must never queue behind sharpening fills.
    uint32_t tail_fills_per_frame = 16;
    // Tier-2 hemisphere-AO enrichments per frame; 0 keeps the enricher loaded
    // but drains nothing.
    uint32_t enrich_per_frame = 2;
    // CPU mesh-copy budget. Rejections past this fall back to the legacy
    // per-material path (i.e. the authored surfaces() tape is ignored).
    uint32_t mesh_budget_mb = 1024;
    // Indirection-table arena. 64 MiB = 16.7M entries.
    uint32_t indirection_mb = 64;
};

// The one live instance every VtResidency reads.
inline VtResidencyBudgets& vt_residency_budgets() {
    static VtResidencyBudgets s;
    return s;
}

inline const props::Group& vt_residency_budgets_group() {
    using props::prop;
    static const auto def = props::group<VtResidencyBudgets>(
        "vt.residency", "Chart VT Budgets",
        prop(&VtResidencyBudgets::max_variants, "max_variants")
            .label("Max variants").range(4.0f, 65534.0f)
            .env("MATTER_VT_MAX_VARIANTS").read_only()
            .doc("Variant record table size. Consumed once at renderer init — "
                 "set MATTER_VT_MAX_VARIANTS before launch to change it."),
        prop(&VtResidencyBudgets::fills_per_frame, "fills_per_frame")
            .label("Fills / frame").range(1.0f, 64.0f)
            .env("MATTER_VT_FILLS_PER_FRAME"),
        prop(&VtResidencyBudgets::tail_fills_per_frame, "tail_fills_per_frame")
            .label("Tail fills / frame").range(1.0f, 64.0f)
            .env("MATTER_VT_TAIL_FILLS_PER_FRAME"),
        prop(&VtResidencyBudgets::enrich_per_frame, "enrich_per_frame")
            .label("Enrich / frame").range(0.0f, 16.0f)
            .env("MATTER_VT_ENRICH_PER_FRAME")
            .doc("Tier-2 AO enrichment rate. 0 disables the tier without "
                 "unloading the enricher."),
        prop(&VtResidencyBudgets::mesh_budget_mb, "mesh_budget_mb")
            .label("CPU mesh budget").range(1.0f, 16384.0f).units("MB")
            .env("MATTER_VT_MESH_BUDGET_MB"),
        prop(&VtResidencyBudgets::indirection_mb, "indirection_mb")
            .label("Indirection arena").range(1.0f, 1024.0f).units("MB")
            .env("MATTER_VT_INDIRECTION_MB").read_only()
            .doc("Consumed once at renderer init — set "
                 "MATTER_VT_INDIRECTION_MB before launch to change it."));
    return def.group();
}

// Layer 5 for engine-standalone paths (headless tests, tools) that never bind
// a registry. Idempotent, and harmless to repeat after the editor's own
// apply_env — both write the same values from the same environment.
inline void ensure_vt_residency_env_applied() {
    static const bool once = [] {
        props::apply_env(&vt_residency_budgets(), vt_residency_budgets_group());
        return true;
    }();
    (void)once;
}

}  // namespace matter
