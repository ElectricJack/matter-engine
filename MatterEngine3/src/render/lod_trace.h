#pragma once

#include <cstdint>
#include <vector>

// The fly-through LOD determinism trace (docs/superpowers/plans/
// 2026-08-04-lod-vt-migration.md, "The fly-through determinism test (M1d)").
//
// What it records is the DRAW AUTHORITY'S OWN ANSWER, not a CPU mirror's
// prediction: VkSceneRenderer reads back the indirect draw commands and the
// draw-transform array a completed frame's cull.comp dispatch wrote, pairs each
// occupied transform slot back to its `cluster_index * kVkMaxLod + lod` bucket,
// and hands the resulting (instance_token, cluster_index, lod) list here.
//
// The emitted stream is a CHANGE-ONLY event log, shaped after MATTER_SKIN_PROBE
// in vk_scene_renderer.cpp: an env latch, a census built per frame, and output
// only where it differs from the previous frame. Two warm runs of the same
// scripted camera path must produce identical streams; MatterEngine3/tools/
// lod_trace_diff.py is the comparator.
//
// Deliberate properties, each of which the design says would otherwise give a
// permanently red or permanently green gate:
//   * events are sorted by (instance_token, cluster_index) before emission,
//     because transform slots come from an atomicAdd and their ORDER races even
//     when the selection is identical;
//   * a pair that stops drawing emits `-` as its new value rather than simply
//     vanishing, so a culling change cannot read as a storm of rung changes;
//   * each line is stamped with the CAMERA PATH POSE INDEX, not the renderer's
//     frame serial. The design only asked for switch ORDER, on the grounds that
//     streaming and publish timing differ run to run even warm — and the serial
//     measurably does (the same first switch landed on serial 37 in one run and
//     36 in the next). The pose index does not: MATTER_CAM_PATH consumes one
//     pose per RENDERED frame, so pose k is pose k however long the world took
//     to become drawable. Stamping with it makes two warm runs byte-identical
//     and buys back the case order alone cannot see — a uniform rescale of
//     `reach` or the budget, which moves every switch without reordering any of
//     them. `lod_trace_diff.py --ignore-frames` falls back to order-only;
//   * `instance_token == 0` is reported loudly (stderr plus a `!` line the
//     comparator fails on), because the token falls back to `source_index + 1`,
//     an order-dependent key that would look perfectly stable while meaning
//     nothing;
//   * a census line carries the drawn-pair / token / cluster counts, WITHOUT
//     WHICH THE GATE IS SATISFIABLE BY RENDERING NOTHING — two runs that both
//     draw an empty world diff identically. The comparator fails a trace whose
//     census never rises above zero.
namespace viewer::lod_trace {

// One (instance, cluster) pair the cull dispatch emitted, with the rung it
// chose for it.
struct Entry {
    uint32_t instance_token = 0;
    uint32_t cluster_index = 0;
    uint32_t lod = 0;
};

// MATTER_LOD_TRACE=<file>. Latched once on first call, like MATTER_SKIN_PROBE;
// everything below is inert when this is false, so the production path pays one
// predictable branch per frame.
bool enabled();

// Capture gate. Defaults to OPEN so `MATTER_LOD_TRACE` alone traces from the
// first drawn frame. The editor closes it at startup when MATTER_CAM_PATH is
// set and reopens it on the frame the scripted path begins, which keeps the
// world's streaming-in churn — the genuinely nondeterministic part of a warm
// run — out of the compared stream.
void set_capture_enabled(bool value);
bool capture_enabled();

// The frame CLOCK the stream is stamped with. The editor sets it to the
// MATTER_CAM_PATH pose index, which is run-independent by construction (one
// pose per rendered frame), unlike the renderer's frame serial: how many frames
// a world takes to become drawable varies between warm runs, so the same switch
// lands on serial 37 in one run and 36 in the next. Nothing calls this when no
// path is driving, and then `frame_label` returns the caller's fallback.
void set_frame_label(uint64_t label);
uint64_t frame_label(uint64_t fallback);

// One completed frame's drawn set, stamped with the label that was current when
// the frame recorded its cull dispatch. Ownership is taken; entries need not be
// sorted or deduplicated.
void submit_frame(uint64_t frame_label, std::vector<Entry> entries);

// Writes the run summary and closes the file. Idempotent.
void close();

}  // namespace viewer::lod_trace
