// draw_overrides_tests.cpp — per-module draw overrides (WS3).
//
// Covers the three halves that are testable without a GPU:
//   1. the sparse module table and its "a default value erases the entry" rule
//   2. DrawOverrideResolver: module -> part-hash resolution, the hidden memo,
//      the incremental catalog path, and what a change reports back to the
//      instance-expansion memo
//   3. the property-system bridge: field naming/defaults, read-back clamping,
//      and the sparse persistence that falls out of a neutral baseline
// plus a CPU mirror of the cull.comp arithmetic, which is what pins the
// default state to bit-exactness.

#include "check.h"

#include "matter/draw_overrides.h"
#include "matter/json_doc.h"
#include "matter/props.h"

#include <cmath>
#include <string>
#include <vector>

using namespace matter;

namespace {

ModuleDrawOverride make_override(bool hide, float dist, float bias) {
    ModuleDrawOverride o;
    o.hide = hide;
    o.max_draw_distance = dist;
    o.lod_bias = bias;
    return o;
}

// ---------------------------------------------------------------------------
// 1. The sparse table
// ---------------------------------------------------------------------------
void test_table_sparsity() {
    DrawOverrideTable t;
    CHECK(t.empty(), "table: starts empty");

    t.set("Tree", make_override(false, 400.0f, 1.0f));
    CHECK(t.size() == 1, "table: a non-default entry is stored");
    CHECK(t.find("Tree") != nullptr, "table: lookup by module");
    CHECK(t.find("Rock") == nullptr, "table: an untouched module has no entry");

    // Editing back to neutral must ERASE, not store a neutral entry: the entry
    // set is what the panel counts and what the resolver walks.
    t.set("Tree", ModuleDrawOverride{});
    CHECK(t.empty(), "table: a default value erases the entry");

    t.set("Grass", make_override(true, 0.0f, 1.0f));
    CHECK(t.hidden_modules().count("Grass") == 1, "table: hidden set");
    t.set("Rock", make_override(false, 0.0f, 0.5f));
    CHECK(t.hidden_modules().size() == 1,
          "table: a bias-only entry is not hidden");
    CHECK(draw_override_has_gpu_effect(*t.find("Rock")),
          "table: a bias reaches the GPU lane");
    CHECK(!draw_override_has_gpu_effect(*t.find("Grass")),
          "table: hide alone never reaches the GPU lane");
}

// ---------------------------------------------------------------------------
// 2. Resolution onto part hashes
// ---------------------------------------------------------------------------
void test_resolver_default_state_is_inert() {
    DrawOverrideResolver r;
    r.add_module(0x11, "Tree");
    r.add_module(0x12, "Grass");
    CHECK(!r.any_hidden(), "resolver: nothing hidden by default");
    CHECK(!r.hidden(0x11), "resolver: an uncatalogued default hides nothing");
    CHECK(r.gpu_entries().empty(),
          "resolver: the GPU lane is empty with no overrides");
    // An unknown hash is not an error, it is simply not overridden — which is
    // the case for every per-sector WorldSector root.
    CHECK(!r.hidden(0xdeadbeef), "resolver: an unknown hash is never hidden");
}

void test_resolver_module_to_hash() {
    DrawOverrideResolver r;
    r.add_module(0x11, "Tree");
    r.add_module(0x12, "Tree");   // a second parametric variant of one module
    r.add_module(0x21, "Grass");
    r.add_module(0x31, "Rock");
    const std::vector<std::string> mods = r.modules();
    CHECK(mods.size() == 3, "resolver: variants of one module collapse to one row");
    CHECK(mods[0] == "Grass" && mods[1] == "Rock" && mods[2] == "Tree",
          "resolver: module list is sorted and deduplicated");

    DrawOverrideTable t;
    t.set("Tree", make_override(true, 0.0f, 1.0f));
    CHECK(r.set_table(t), "resolver: hiding a module reports a hidden change");
    CHECK(r.any_hidden(), "resolver: any_hidden tracks the table");
    CHECK(r.hidden(0x11) && r.hidden(0x12),
          "resolver: EVERY variant of a hidden module is hidden");
    CHECK(!r.hidden(0x21), "resolver: a sibling module is untouched");
    CHECK(r.gpu_entries().empty(), "resolver: hide alone leaves the GPU lane empty");

    // A distance/bias-only edit must NOT report a hidden change: it is consumed
    // by the GPU lane and needs no instance-expansion rebuild.
    t.set("Grass", make_override(false, 90.0f, 0.5f));
    CHECK(!r.set_table(t),
          "resolver: a distance/bias edit does not retire the expansion memo");
    CHECK(r.gpu_entries().size() == 1, "resolver: one GPU entry for Grass");
    CHECK(r.gpu_entries()[0].part_hash == 0x21, "resolver: the GPU entry's hash");
    CHECK(r.gpu_entries()[0].value.max_draw_distance == 90.0f &&
              r.gpu_entries()[0].value.lod_bias == 0.5f,
          "resolver: the GPU entry's values");

    // Unhiding is a hidden change in the other direction.
    t.set("Tree", ModuleDrawOverride{});
    CHECK(r.set_table(t), "resolver: unhiding also retires the memo");
    CHECK(!r.hidden(0x11), "resolver: the memo did not survive the change");
}

void test_resolver_gpu_entries_sorted_and_incremental() {
    DrawOverrideResolver r;
    r.add_module(0x30, "Rock");
    r.add_module(0x10, "Tree");
    DrawOverrideTable t;
    t.set("Tree", make_override(false, 500.0f, 1.0f));
    t.set("Rock", make_override(false, 0.0f, 2.0f));
    r.set_table(t);
    CHECK(r.gpu_entries().size() == 2, "resolver: both modules in the lane");
    CHECK(r.gpu_entries()[0].part_hash < r.gpu_entries()[1].part_hash,
          "resolver: the GPU lane is sorted by hash (the renderer binary-searches)");
    CHECK(r.consume_gpu_dirty(), "resolver: a table change dirties the lane");
    CHECK(!r.consume_gpu_dirty(), "resolver: consuming clears the flag");

    // A newly installed variant of an already-overridden module joins the lane
    // without a full rebuild — this is the streaming case (a world installs a
    // module's variants as sectors demand them).
    CHECK(r.add_module(0x20, "Tree"), "resolver: a new variant is new");
    CHECK(r.consume_gpu_dirty(), "resolver: a new overridden variant dirties the lane");
    CHECK(r.gpu_entries().size() == 3, "resolver: the variant joined the lane");
    CHECK(r.gpu_entries()[0].part_hash == 0x10 &&
              r.gpu_entries()[1].part_hash == 0x20 &&
              r.gpu_entries()[2].part_hash == 0x30,
          "resolver: the incremental insert kept the lane sorted");

    // A new variant of a module with NO override must not touch the lane.
    CHECK(r.add_module(0x40, "Grass"), "resolver: an unrelated variant is new");
    CHECK(!r.consume_gpu_dirty(),
          "resolver: an un-overridden variant leaves the lane alone");

    // A hidden module's newly installed variant must drop its stale memo.
    t.set("Grass", make_override(true, 0.0f, 1.0f));
    r.set_table(t);
    CHECK(r.hidden(0x40), "resolver: the new variant hides too");
    r.add_module(0x41, "Grass");
    CHECK(r.hidden(0x41), "resolver: a later variant of a hidden module hides");
}

// ---------------------------------------------------------------------------
// 3. The property-system bridge
// ---------------------------------------------------------------------------
void test_field_naming() {
    std::string module, field;
    CHECK(split_draw_override_field("AlpineGrass/max_dist", module, field) &&
              module == "AlpineGrass" && field == "max_dist",
          "naming: module/field split");
    CHECK(!split_draw_override_field("nomodule", module, field),
          "naming: a name with no separator is not a draw-override field");
    CHECK(!split_draw_override_field("/hide", module, field),
          "naming: an empty module is rejected");
    CHECK(!split_draw_override_field(nullptr, module, field),
          "naming: null is rejected");

    // The FIFO `set` path splits a full path on its LAST '.', so no field name
    // may contain one — that is why the separator is '/'.
    CHECK(std::string("Tree/hide").find('.') == std::string::npos,
          "naming: field names carry no '.' (resolve_field splits on it)");
}

void test_group_defaults_and_readback() {
    const std::vector<std::string> modules = {"Grass", "Tree"};
    std::unique_ptr<props::DynamicGroup> g = build_draw_override_group(modules);
    CHECK(g != nullptr, "group: built");
    CHECK(g->field_count() == 6, "group: three fields per module");
    CHECK(std::string(g->path()) == kDrawOverridesPath, "group: registry path");
    CHECK(g->index_of("Tree/hide") >= 0 && g->index_of("Tree/max_dist") >= 0 &&
              g->index_of("Tree/lod_bias") >= 0,
          "group: all three fields exist per module");

    CHECK(build_draw_override_group({}) == nullptr,
          "group: a world with no modules declares no group");

    // Declared defaults ARE the neutral values, which is what makes the layer-2
    // baseline "no overrides" and the sparse save write only real edits.
    DrawOverrideTable t;
    read_draw_override_group(*g, t);
    CHECK(t.empty(), "group: the declared defaults read back as NO overrides");

    g->set_bool(static_cast<uint32_t>(g->index_of("Grass/hide")), true);
    g->set_float(static_cast<uint32_t>(g->index_of("Tree/max_dist")), 250.0f);
    g->set_float(static_cast<uint32_t>(g->index_of("Tree/lod_bias")), 0.5f);
    read_draw_override_group(*g, t);
    CHECK(t.size() == 2, "group: two modules read back");
    CHECK(t.find("Grass")->hide, "group: hide round-trips");
    CHECK(t.find("Tree")->max_draw_distance == 250.0f, "group: distance round-trips");
    CHECK(t.find("Tree")->lod_bias == 0.5f, "group: bias round-trips");
    CHECK(!t.find("Tree")->hide, "group: an unset field stays neutral");

    // max_dist is an UNRANGED drag so 0 ("unlimited") stays reachable; the
    // clamp therefore lives on the read side, not in the schema.
    const uint32_t dist = static_cast<uint32_t>(g->index_of("Tree/max_dist"));
    CHECK(!g->field(dist).has_range,
          "group: max_dist is unranged so 0 = unlimited is expressible");
    g->set_float(dist, -50.0f);
    read_draw_override_group(*g, t);
    CHECK(t.find("Tree")->max_draw_distance == 0.0f,
          "group: a negative distance clamps to unlimited on read");

    // lod_bias IS ranged, so the setter itself clamps.
    const uint32_t bias = static_cast<uint32_t>(g->index_of("Tree/lod_bias"));
    CHECK(g->field(bias).has_range, "group: lod_bias is a ranged slider");
    g->set_float(bias, 99.0f);
    CHECK(g->get_float(bias) == kDrawOverrideMaxLodBias,
          "group: the ranged setter clamps the bias");
}

void test_sparse_persistence() {
    std::unique_ptr<props::DynamicGroup> g =
        build_draw_override_group({"Grass", "Tree"});
    props::Registry reg;
    props::Binding* b = reg.get(g->bind_into(reg, props::Scope::World));
    CHECK(b != nullptr, "persist: bound");
    // bind_into captures the neutral declared defaults as the baseline; the
    // editor deliberately never re-captures it at a later connect, so this is
    // the state every sparse save diffs against.
    CHECK(props::is_group_default(*b), "persist: the baseline is neutral");

    matter::jsondoc::Value empty_doc;
    props::save_scope(reg, props::Scope::World, empty_doc);
    const std::string empty_out = matter::jsondoc::write_json(empty_doc);
    CHECK(empty_out.find("Grass") == std::string::npos &&
              empty_out.find("Tree") == std::string::npos,
          "persist: an all-default group writes nothing (replay-safe)");

    g->set_bool(static_cast<uint32_t>(g->index_of("Grass/hide")), true);
    matter::jsondoc::Value doc;
    props::save_scope(reg, props::Scope::World, doc);
    const std::string out = matter::jsondoc::write_json(doc);
    CHECK(out.find("Grass/hide") != std::string::npos,
          "persist: only the edited module's edited field is written");
    CHECK(out.find("Grass/max_dist") == std::string::npos &&
              out.find("Tree/hide") == std::string::npos,
          "persist: neutral siblings stay out of the file");

    // The FIFO `set draw.overrides.<Module>/<field>` path resolves.
    props::Binding* found = nullptr;
    const props::Desc* desc = nullptr;
    CHECK(props::resolve_field(reg, "draw.overrides.Tree/max_dist", found, desc),
          "persist: resolve_field splits group and module/field");
    CHECK(found == b && std::string(desc->name) == "Tree/max_dist",
          "persist: resolve_field lands on the right field");
    CHECK(props::parse_and_set(*b, *desc, "320"), "persist: text parse");
    CHECK(g->get_float(static_cast<uint32_t>(g->index_of("Tree/max_dist"))) == 320.0f,
          "persist: the parsed value reached the buffer");

    // Reset restores the neutral baseline, which is also "Reset all" in the UI.
    props::reset_group(*b);
    DrawOverrideTable t;
    read_draw_override_group(*g, t);
    CHECK(t.empty(), "persist: reset returns the whole group to no-overrides");

    g->unbind_from(reg);
}

// ---------------------------------------------------------------------------
// 4. CPU mirror of the cull.comp arithmetic
//
// The enforcement point for max_draw_distance / lod_bias is
// shaders_vk/cull.comp, which cannot run headless. What CAN be pinned here is
// the arithmetic the shader performs, in the same order, so the bit-exactness
// claim for the default state is a checked property and not a comment.
// ---------------------------------------------------------------------------

// The pre-override expression, verbatim from cull.comp.
float baseline_projected_size(float local_radius, float scale,
                              float distance_to_eye, float pixel_budget) {
    return local_radius * scale / distance_to_eye * pixel_budget;
}

// The post-override expression, with the same guards the shader uses.
bool overridden_projected_size(float local_radius, float scale,
                               float distance_to_eye, float pixel_budget,
                               const PartDrawOverrideGpu& ov, float& out) {
    if (ov.max_draw_distance > 0.0f && distance_to_eye > ov.max_draw_distance)
        return false;   // culled
    float projected = local_radius * scale / distance_to_eye * pixel_budget;
    if (ov.lod_bias != 1.0f) projected *= ov.lod_bias;
    out = projected;
    return true;
}

// cull.comp's selection loop: first rung whose threshold the size clears.
uint32_t select_rung(float size, const std::vector<float>& thresholds) {
    uint32_t lod = static_cast<uint32_t>(thresholds.size()) - 1u;
    for (uint32_t i = 0; i < thresholds.size(); ++i)
        if (size >= thresholds[i]) return i;
    return lod;
}

void test_cull_math_default_is_bit_exact() {
    const std::vector<float> thresholds = {0.5f, 0.25f, 0.1f, 0.02f};
    const PartDrawOverrideGpu neutral{};   // {0, 1} — the one-entry dummy table
    CHECK(neutral.max_draw_distance == 0.0f && neutral.lod_bias == 1.0f,
          "cull: the neutral entry is {0, 1}");

    for (float d = 1.0f; d < 4000.0f; d *= 1.37f) {
        for (float budget : {0.05f, 0.37f, 1.0f, 2.5f, 4.0f}) {
            const float want = baseline_projected_size(1.3f, 0.9f, d, budget);
            float got = 0.0f;
            CHECK(overridden_projected_size(1.3f, 0.9f, d, budget,
                                                   neutral, got),
                         "cull: the neutral entry never culls");
            CHECK(got == want,
                         "cull: the neutral entry is a bit-exact no-op");
            CHECK(select_rung(got, thresholds) ==
                             select_rung(want, thresholds),
                         "cull: the neutral entry selects the same rung");
        }
    }
    CHECK(true, "cull: neutral overrides reproduce the pre-override math exactly");
}

void test_cull_math_overrides() {
    const std::vector<float> thresholds = {0.5f, 0.25f, 0.1f, 0.02f};
    PartDrawOverrideGpu ov;
    ov.max_draw_distance = 100.0f;
    ov.lod_bias = 1.0f;
    float out = 0.0f;
    CHECK(overridden_projected_size(1.0f, 1.0f, 99.9f, 1.0f, ov, out),
          "cull: inside the cap, the instance survives");
    CHECK(!overridden_projected_size(1.0f, 1.0f, 100.1f, 1.0f, ov, out),
          "cull: past the cap, the instance is culled");
    CHECK(overridden_projected_size(1.0f, 1.0f, 100.0f, 1.0f, ov, out),
          "cull: the cap is inclusive (strictly-greater rejects)");

    // The cap is a DISTANCE test, not a size test, so it is independent of the
    // pixel budget — which is exactly what makes it a predictable authoring
    // control rather than a second budget knob.
    ov.max_draw_distance = 50.0f;
    CHECK(!overridden_projected_size(100.0f, 1.0f, 60.0f, 4.0f, ov, out),
          "cull: a huge part past the cap is still culled");

    // Bias scales the projected size exactly where the pixel budget does, so a
    // bias of b is indistinguishable from a budget scaled by b.
    ov.max_draw_distance = 0.0f;
    ov.lod_bias = 0.5f;
    overridden_projected_size(1.0f, 1.0f, 10.0f, 1.0f, ov, out);
    CHECK(out == baseline_projected_size(1.0f, 1.0f, 10.0f, 0.5f),
          "cull: bias b == pixel budget scaled by b");

    // A bias below 1 can only move the selection COARSER (a higher index).
    ov.lod_bias = 0.25f;
    float biased = 0.0f;
    overridden_projected_size(1.0f, 1.0f, 5.0f, 1.0f, ov, biased);
    const float plain = baseline_projected_size(1.0f, 1.0f, 5.0f, 1.0f);
    CHECK(select_rung(biased, thresholds) >= select_rung(plain, thresholds),
          "cull: a bias < 1 never picks a finer rung");
    ov.lod_bias = 4.0f;
    overridden_projected_size(1.0f, 1.0f, 5.0f, 1.0f, ov, biased);
    CHECK(select_rung(biased, thresholds) <= select_rung(plain, thresholds),
          "cull: a bias > 1 never picks a coarser rung");
}

// The renderer's own last hop, hash -> dense part_slot, mirrored here because
// vk_scene_renderer.cpp needs a device to instantiate.
void test_part_slot_table_shape() {
    DrawOverrideResolver r;
    r.add_module(0xA0, "Tree");
    r.add_module(0xB0, "Grass");
    DrawOverrideTable t;
    t.set("Grass", make_override(false, 40.0f, 1.0f));
    r.set_table(t);

    // parts_ in slot order; slot 1 is a released record (hash 0).
    const uint64_t slot_hash[] = {0xA0, 0, 0xB0};
    std::vector<PartDrawOverrideGpu> table(3);
    for (size_t slot = 0; slot < 3; ++slot) {
        if (slot_hash[slot] == 0) continue;
        for (const PartDrawOverrideEntry& e : r.gpu_entries())
            if (e.part_hash == slot_hash[slot]) table[slot] = e.value;
    }
    CHECK(table[0].max_draw_distance == 0.0f && table[0].lod_bias == 1.0f,
          "slots: an un-overridden part slot stays neutral");
    CHECK(table[1].lod_bias == 1.0f, "slots: a released slot stays neutral");
    CHECK(table[2].max_draw_distance == 40.0f,
          "slots: the overridden part slot carries its cap");
}

}  // namespace

int main() {
    test_table_sparsity();
    test_resolver_default_state_is_inert();
    test_resolver_module_to_hash();
    test_resolver_gpu_entries_sorted_and_incremental();
    test_field_naming();
    test_group_defaults_and_readback();
    test_sparse_persistence();
    test_cull_math_default_is_bit_exact();
    test_cull_math_overrides();
    test_part_slot_table_shape();
    return check_summary();
}
