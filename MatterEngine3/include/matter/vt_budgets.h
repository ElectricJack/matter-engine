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
    // M6.5: directional-occlusion bakes per frame (the SECOND tier — coarse
    // far-field pages, impostored props as casters). Its own budget rather
    // than a share of enrich_per_frame: the two tiers admit opposite page
    // sizes, and one budget would let a burst of fine pages starve the far
    // field indefinitely. Default 0 = tier off.
    uint32_t dir_occ_per_frame = 0;
    // CPU mesh-copy budget. Rejections past this fall back to the legacy
    // per-material path (i.e. the authored surfaces() tape is ignored).
    uint32_t mesh_budget_mb = 1024;
    // Indirection-table arena. 64 MiB = 16.7M entries.
    uint32_t indirection_mb = 64;

    // ---- Workstream-2 additions -------------------------------------------
    // Physical page pool, in pages. Rounded up to whole array layers by
    // VtResidency::init, which then fails the init when the layer count exceeds
    // the device's maxImageArrayLayers. That DEVICE check is why this used to
    // be described as inexpressible in a schema — but the value's own bounds
    // are static (one layer .. 256 layers) and the device check stays exactly
    // where it was, so it describes fine. Init-consumed: ReadOnly.
    uint32_t pool_pages = 8192;
    // Frames a physical slot is protected from eviction after its last use.
    // One frame is not enough: DLSS jitter shifts which feedback blocks sample
    // which pages, so a hot page is requested only every few frames. Re-read
    // by refresh_budgets every begin_frame, so this one is live.
    uint32_t evict_protect_frames = 16;
    // Demand-driven working set. linger_frames is how long a variant survives
    // unwanted before its layer is reclaimed; requests_per_frame bounds the
    // registration requests the renderer's demand pass surfaces to the engine.
    // Both are read per demand pass (once per frame), so both are live.
    uint32_t linger_frames = 240;
    uint32_t requests_per_frame = 16;
    // Milliseconds of registration servicing per frame. Bounds how many
    // requests START, never how long one takes — at least one is always
    // serviced, so the VT cannot stall. 0 restores service-everything.
    float request_budget_ms = 4.0f;
};

// Tier-2 hemisphere-AO enrichment settings (vt_enrich.cpp). Six MATTER_VT_
// ENRICH_* vars that were read through a private env_u32/env_f32 pair into
// VtEnricher::Impl at init; five of them are per-batch push-constant inputs and
// so are genuinely live, and the sixth sizes a descriptor pool at init.
//
// Kept a SEPARATE struct from VtResidencyBudgets rather than folded into it:
// these are the enricher's own parameters (a subsystem that may not exist at
// all — no hardware ray tracing means no VtEnricher), while the budgets above
// are the residency layer's. Two structs, two groups, one env pass each.
struct VtEnrichSettings {
    // Rays per texel.
    uint32_t samples = 32;
    // Occlusion blend factor. 0 leaves pages at their tier-1 content.
    float strength = 1.0f;
    // Ray length cap, as a multiple of the page texel footprint...
    float cap_texels = 4.0f;
    // ...and as an absolute distance. The shorter of the two wins.
    float cap_meters = 0.5f;
    // Occlusion floor: a fully enclosed texel never darkens past this.
    float min_ao = 0.15f;
    // Cached (variant, rung) acceleration structures. Sizes the descriptor
    // pool at init, so it is not live-editable.
    uint32_t as_cache = 8;
    // ---- M6.5 directional tier (vt_dirocc.comp) --------------------------
    // Smallest page-texel footprint (metres) admitted to the directional
    // bake. This is the INVERSE of the contact tier's skip: dir-occ runs on
    // coarse far-field pages only, and the default is chosen so the tier
    // admits exactly the pages the ~2.0 m contact fade-out rejects — the two
    // admission rules tile the mip range with no gap and no overlap.
    float dirocc_min_footprint = 2.0f;
    // How far (metres) a caster's occlusion reaches. Feeds both ends: the
    // shader's ray length AND the harvest's receiver-expansion overlap test
    // (vt_caster_select.h), which MUST use the same number or shadows stop
    // exactly at the boundary between "harvested" and "traced".
    float dirocc_reach = 64.0f;
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
        prop(&VtResidencyBudgets::dir_occ_per_frame, "dir_occ_per_frame")
            .label("Dir-occ / frame").range(0.0f, 16.0f)
            .env("MATTER_VT_DIROCC_PER_FRAME")
            .doc("M6.5 directional-occlusion bake rate: impostored props "
                 "cast into the terrain sector's own pages, so a tree keeps "
                 "its shadow after it becomes a billboard. Coarse far-field "
                 "pages only. 0 disables the tier."),
        prop(&VtResidencyBudgets::mesh_budget_mb, "mesh_budget_mb")
            .label("CPU mesh budget").range(1.0f, 16384.0f).units("MB")
            .env("MATTER_VT_MESH_BUDGET_MB"),
        prop(&VtResidencyBudgets::indirection_mb, "indirection_mb")
            .label("Indirection arena").range(1.0f, 1024.0f).units("MB")
            .env("MATTER_VT_INDIRECTION_MB").read_only()
            .doc("Consumed once at renderer init — set "
                 "MATTER_VT_INDIRECTION_MB before launch to change it."),
        // 64 pages per array layer .. 256 layers — the same static bounds the
        // local env_u32 read enforced. The device's maxImageArrayLayers check
        // stays in VtResidency::init where the device is in scope.
        prop(&VtResidencyBudgets::pool_pages, "pool_pages")
            .label("Page pool").range(64.0f, 16384.0f)
            .env("MATTER_VT_POOL_PAGES").read_only()
            .doc("Physical page slots, rounded up to whole array layers. "
                 "Allocated once at renderer init — set MATTER_VT_POOL_PAGES "
                 "before launch to change it."),
        prop(&VtResidencyBudgets::evict_protect_frames, "evict_protect_frames")
            .label("Evict protect").range(1.0f, 1024.0f).units("frames")
            .env("MATTER_VT_EVICT_PROTECT_FRAMES")
            .doc("Frames a page slot is safe from eviction after its last use. "
                 "Below the DLSS jitter cycle an oversubscribed pool "
                 "ping-pongs pages instead of settling."),
        prop(&VtResidencyBudgets::linger_frames, "linger_frames")
            .label("Variant linger").range(2.0f, 100000.0f).units("frames").log()
            .env("MATTER_VT_LINGER_FRAMES")
            .doc("How long a demand-registered variant survives unwanted "
                 "before its layer is reclaimed."),
        prop(&VtResidencyBudgets::requests_per_frame, "requests_per_frame")
            .label("Requests / frame").range(1.0f, 256.0f)
            .env("MATTER_VT_REQUESTS_PER_FRAME")
            .doc("Registration requests the demand pass surfaces per frame."),
        prop(&VtResidencyBudgets::request_budget_ms, "request_budget_ms")
            .label("Request budget").range(0.0f, 32.0f).units("ms")
            .env("MATTER_VT_REQUEST_BUDGET_MS")
            .doc("Per-frame time budget for SERVICING registration requests. "
                 "Bounds how many start, never how long one takes; 0 services "
                 "the whole queue."));
    return def.group();
}

inline VtEnrichSettings& vt_enrich_settings() {
    static VtEnrichSettings s;
    return s;
}

inline const props::Group& vt_enrich_settings_group() {
    using props::prop;
    static const auto def = props::group<VtEnrichSettings>(
        "vt.enrich", "Chart VT Enrichment",
        prop(&VtEnrichSettings::samples, "samples")
            .label("Rays / texel").range(8.0f, 64.0f)
            .env("MATTER_VT_ENRICH_SAMPLES"),
        prop(&VtEnrichSettings::strength, "strength")
            .label("Strength").range(0.0f, 1.0f)
            .env("MATTER_VT_ENRICH_STRENGTH")
            .doc("0 leaves enriched pages at their tier-1 content."),
        prop(&VtEnrichSettings::cap_texels, "cap_texels")
            .label("Ray cap (texels)").range(0.25f, 64.0f)
            .env("MATTER_VT_ENRICH_CAP_TEXELS"),
        prop(&VtEnrichSettings::cap_meters, "cap_meters")
            .label("Ray cap").range(0.02f, 64.0f).units("m").log()
            .env("MATTER_VT_ENRICH_CAP_METERS")
            .doc("Absolute ray-length cap; the shorter of this and the texel "
                 "cap wins. Also scales the residency layer's coarse-page "
                 "skip, which reads it through max_footprint_meters()."),
        prop(&VtEnrichSettings::min_ao, "min_ao")
            .label("Min AO").range(0.0f, 1.0f)
            .env("MATTER_VT_ENRICH_MIN_AO")
            .doc("Occlusion floor: a fully enclosed texel never darkens past "
                 "this."),
        prop(&VtEnrichSettings::as_cache, "as_cache")
            .label("AS cache").range(1.0f, 64.0f)
            .env("MATTER_VT_ENRICH_AS_CACHE").read_only()
            .doc("Cached (variant, rung) acceleration structures. Sizes a "
                 "descriptor pool at enricher init — set "
                 "MATTER_VT_ENRICH_AS_CACHE before launch to change it."),
        prop(&VtEnrichSettings::dirocc_min_footprint, "dirocc_min_footprint")
            .label("Dir-occ min footprint").range(0.01f, 1000.0f).units("m")
            .log()
            .env("MATTER_VT_DIROCC_MIN_FOOTPRINT")
            .doc("Smallest page-texel size the directional tier bakes. The "
                 "INVERSE of the contact tier's coarse-page skip: dir-occ "
                 "admits coarse far-field pages only."),
        prop(&VtEnrichSettings::dirocc_reach, "dirocc_reach")
            .label("Dir-occ reach").range(1.0f, 1024.0f).units("m").log()
            .env("MATTER_VT_DIROCC_REACH")
            .doc("Caster occlusion range: the bake's ray length and the "
                 "harvest's cross-sector overlap expansion, which must "
                 "agree or shadows stop at the harvest boundary."));
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

// The same for the enrichment settings. Separate latch, separate group: the
// enricher is created (and its env read) only on a ray-tracing-capable device,
// and folding the two would make a no-RT run apply an env pass for a subsystem
// that never exists.
inline void ensure_vt_enrich_env_applied() {
    static const bool once = [] {
        props::apply_env(&vt_enrich_settings(), vt_enrich_settings_group());
        return true;
    }();
    (void)once;
}

}  // namespace matter
