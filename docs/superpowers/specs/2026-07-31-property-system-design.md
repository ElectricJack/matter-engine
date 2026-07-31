# Property System Design

**Date:** 2026-07-31
**Status:** Proposed
**Scope:** MatterEngine3 core + MatterEditor integration; script-authored properties sketched as a later phase

## 1. Problem

Tunable values are scattered across six ad-hoc mechanisms with no common definition,
edit, or persistence story:

- ~25–30 settings structs with member-initializer defaults (`FogSettings`,
  `VulkanVolumetricsSettings`, `TilesetPomSettings`, `RenderOptions`,
  `matter_stream::Config`, `lod_bake::BakeTargets`, …) — memory-only, recompiled to change defaults.
- ~87 `MATTER_*` env vars, most read in function-local statics at process start,
  with the parse helper copy-pasted three times (`vt_residency.cpp:23`,
  `vt_enrich.cpp:45`, `vk_scene_renderer.cpp:43`).
- `ViewerStats` (`MatterEditor/src/ui.h:67-168`) — a ~100-field bag mixing stats and
  ~10 hand-wired tunables, reset to struct defaults on every world switch.
- Hand-written ImGui sliders bound field-by-field (`ui.cpp:955-1002`, `ui.cpp:584-866`),
  duplicated bindings (`pixel_budget` has two edit sites), values propagated to the
  engine by a hand-written copy block (`main.cpp:2552-2562`).
- The ECS `FieldDescriptor` tables (`scene_registry.cpp:24-94`) — the one data-driven
  pattern — but field *access* is a `strcmp` ladder in `main.cpp:97-350` because
  descriptors carry no offsets, and enum labels live in a hardcoded table in
  `properties_panel.cpp:92-99`.
- Nothing persists except part-param content hashes, `workbench_manifest.json`, and
  `imgui.ini`. Every editor tuning session is lost on exit.

We want: define a property once in code (later: in scripts), get it edited in the
editor automatically, and have edits persist to disk with sane precedence.

## 2. Goals

1. **One definition site.** A property's name, type, range, default, doc, and flags are
   declared once, next to the struct that owns it. No parallel edit-site code.
2. **Zero hot-path cost.** Engine code keeps reading plain struct members. The system
   is a *schema over existing structs*, not a lookup-through-registry cvar store.
3. **Auto UI.** A generic ImGui renderer draws any registered group; existing
   hand-written panels are migrated to it incrementally.
4. **Persistence with layers.** `compiled default → world JS authored → persisted
   override file → env var → live edit`, formalizing the precedence chain that
   `make_streaming_profile` (`matter_engine.cpp:334-401`) already implements ad hoc.
5. **Editor-only ImGui.** The core is ImGui-free and lives in MatterEngine3, mirroring
   the existing `scene_registry` (engine) / `properties_registry` (editor) split.
6. **Script-ready.** The same schema model must be constructible at runtime from a
   QuickJS object so world/part scripts can declare properties later (Phase 4).

### Non-goals (v1)

- Replacing part `static params`. Those are **bake inputs**: canonical-JSON-hashed into
  the content address (`script_host.cpp:576-646`, `part_graph.cpp:51-114`). Editing one
  means re-baking, which is the Part Workbench's job. The property system handles
  **runtime tunables**; the two stay distinct (see §9).
- Writing edits back into authored `.js` source. Defaults stay authored in JS; editor
  edits land in sidecar JSON. The `// @matter-data` marker round-trip from the
  2026-07-17 world-as-JS spec remains a later option (see Open Questions).
- Undo/redo integration.
- Migrating all 87 env vars or all 30 structs at once. The system must be adoptable
  one group at a time.

## 3. Core model (`matter::props`, ImGui-free)

New files: `MatterEngine3/include/matter/props.h`, `MatterEngine3/src/props/props.cpp`.

### 3.1 Descriptors

```cpp
namespace matter::props {

enum class Type : uint8_t { Float, Int, UInt, Bool, Enum, Float3, Color3, String };

enum Flags : uint32_t {
    None           = 0,
    Logarithmic    = 1 << 0,  // slider uses ImGuiSliderFlags_Logarithmic
    ReadOnly       = 1 << 1,  // displayed, never editable/persisted
    RequiresReload = 1 << 2,  // edit lands in a draft; applied on world reconnect
    NoSerialize    = 1 << 3,  // editable live, never written to disk
};

struct Desc {
    const char* name = nullptr;        // "phase_g" — JSON key and UI id
    const char* label = nullptr;       // "Phase g" — UI text (nullptr → name)
    Type        type = Type::Float;
    uint32_t    offset = 0;            // byte offset into the owning struct
    float       min = 0, max = 0;
    bool        has_range = false;
    float       step = 0;              // 0 → widget default
    const char* doc = nullptr;         // tooltip
    const char* units = nullptr;       // "m", "EV" — appended to format string
    const char* const* enum_labels = nullptr;
    uint32_t    enum_count = 0;
    const char* env = nullptr;         // optional "MATTER_*" override var
    uint32_t    flags = 0;
};

struct Group {
    const char* path = nullptr;        // "render.volumetrics" — registry + JSON key
    const char* label = nullptr;       // "Volumetrics" — panel header
    const Desc* fields = nullptr;
    uint32_t    field_count = 0;
    uint32_t    struct_size = 0;
    void      (*construct_default)(void*) = nullptr; // placement-new a default instance
};
```

Notably absent: per-field default values. Defaults come from the struct's member
initializers via `construct_default` — the schema never repeats them, so they can't
drift. `is_default(field)` compares against a default-constructed instance;
"Reset" copies the field back from it.

### 3.2 Declaring a schema

A builder keyed on pointer-to-member, so type and offset are deduced and cannot
disagree with the struct:

```cpp
// vulkan_volumetrics_props.cpp (or next to the struct's home TU)
#include "matter/props.h"

static const auto s_volumetrics = matter::props::group<VulkanVolumetricsSettings>(
    "render.volumetrics", "Volumetrics",
    prop(&VulkanVolumetricsSettings::enabled,        "enabled"),
    prop(&VulkanVolumetricsSettings::phase_g,        "phase_g")
        .range(-0.99f, 0.99f).doc("Henyey-Greenstein anisotropy"),
    prop(&VulkanVolumetricsSettings::temporal_blend, "temporal_blend").range(0.f, 1.f),
    prop(&VulkanVolumetricsSettings::fog_density_mul,"fog_density_mul")
        .range(0.f, 8.f).log(),
    prop(&VulkanVolumetricsSettings::fog_falloff_mul,"fog_falloff_mul").range(0.f, 8.f));
```

`prop(&S::member, name)` returns a small constexpr-friendly builder; `group<S>(...)`
collapses the builders into a `static const Desc[]` + `Group` and registers nothing by
itself. Hand-written `static const Desc[]` tables (the existing `scene_registry.cpp`
style) remain a valid escape hatch — the builder is sugar over the same structs.

### 3.3 Registry and binding

Schemas describe types; the registry binds them to live instances:

```cpp
enum class Scope : uint8_t {
    User,      // per-machine editor prefs — gitignored, autosaved
    Project,   // projects/<name>/editor/properties.json — committed, explicit save
    World,     // projects/<name>/editor/worlds/<World>.props.json — committed, explicit save
    Session,   // live-only, never persisted (stats, debug toggles)
};

class Registry {
public:
    // instance must outlive the binding; unbind before destroying it.
    BindingId bind(const Group& schema, void* instance, Scope scope);
    void unbind(BindingId);

    // Capture the post-authored baseline (see §4). Called after world JS +
    // engine defaults have been applied, before override files.
    void capture_baseline(BindingId);

    // Enumeration for the editor panel and the persistence manager.
    // Path lookup for FIFO commands / scripts.
    Binding* find(const char* group_path);
    ...
};
```

Engine subsystems register their groups at init and keep reading their structs
directly — the registry holds `void* + schema`, it is not on any read path.
`ViewerStats`' tunable subsections are bound by the editor at startup; per-world
groups (fog, volumetrics overrides) are bound/unbound on world connect/disconnect.

Generic typed access (`get_float(binding, desc)` / `set_float(...)` switching on
`Type` over `instance + offset`) lives here too — this is what the UI, the
serializer, the env layer, and later scripts all go through, and it is what
replaces the `strcmp` ladder pattern.

**Threading:** identical to today. Edits happen on the main thread between frames;
values reach worker systems the same way they already do (per-frame copy into
`RenderOptions` at `main.cpp:2552-2562`, or at world connect for `RequiresReload`
groups). The system moves no reads onto the registry.

## 4. Layering and baselines

Effective value = last writer in:

```
1. compiled default        (struct member initializers)
2. world JS authored       (world_definition_loader → settings structs)
3. project override file   (Scope::Project)
4. world override file     (Scope::World)
5. env var                 (Desc::env, if set in this process)
6. live edit               (UI / FIFO)
```

Implementation: after layer 2 lands in the bound instance, the world-connect path
calls `capture_baseline`, which memcpys the instance into a side buffer. Then:

- **Load:** apply the project file, then the world file, then env — each writes
  fields directly into the instance.
- **Save (sparse):** diff instance vs baseline; write only differing fields. A field
  edited back to its baseline value drops out of the file. This keeps override files
  small, lets authored JS defaults evolve without stale copies pinning them, and
  makes "Reset to World" (the existing lighting-panel button) a baseline copy.
- **Env-overridden fields** render disabled with an `env` badge — otherwise UI edits
  silently fight the env layer. Env stays the debugging/repro channel
  (`issue_reporter`'s repro lines keep working unchanged).

`Scope::User` groups (camera speed, panel prefs, last world) have no world JS layer;
their baseline is the compiled default.

## 5. On-disk format

JSON, one file per scope:

| Scope | Path | Policy |
|---|---|---|
| User | `MatterEditor/editor_settings.json` (found via `resolve_asset_root`, exe-dir aware) | gitignored, autosaved (debounced ~1 s) |
| Project | `projects/<name>/editor/properties.json` | committed, explicit Save + dirty indicator |
| World | `projects/<name>/editor/worlds/<World>.props.json` | committed, explicit Save + dirty indicator |

`projects/<name>/editor/` is the directory the 2026-07-17 world-as-JS spec reserved
for exactly this; it currently holds only `.gitkeep`.

```json
{
  "version": 1,
  "groups": {
    "render.volumetrics": { "phase_g": 0.62, "fog_density_mul": 1.4 },
    "render.pom": { "steps": 24 }
  }
}
```

Rules:

- **Reader is tolerant:** missing group/field → keep current value; wrong type → skip
  with a console warning (the `shot_replay.cpp:23-94` getter philosophy).
- **Writer preserves unknown content:** the loaded document is kept; saving updates
  known groups in place and leaves unrecognized groups/keys untouched. This protects
  files when switching between branches with different property sets.
- **Atomic writes** via `part_asset::replace_file_atomic_detailed`
  (`part_asset_v2.h:213-224`).
- Integral floats print without a trailing `.0` and object order is preserved — i.e.
  the `part_workbench.cpp:37-257` JSON implementation's behavior.

**Prerequisite refactor:** lift that `JsonValue`/`JsonParser`/writer out of
`part_workbench.cpp`'s anonymous namespace into a shared TU
(proposed: `MatterEngine3/src/util/json_doc.{h,cpp}`) so the engine-side props core
can use it. It is the fourth hand-rolled JSON in the tree; this makes it the last.
`part_graph.cpp`'s canonical `params_to_json`/`params_from_json` is **not** touched —
the bake-cache hash depends on its exact bytes.

## 6. Editor integration (`MatterEditor/src/property_editor.{h,cpp}`)

### 6.1 Generic renderer

`draw_group(Binding&)` — one `switch (desc.type)` producing the widget per field,
generalizing `properties_panel.cpp:365-399`'s `draw_field`:

- `has_range` → Slider (with `Logarithmic` flag honored), else Drag — the existing
  `widget_for_field` policy (`properties_registry.cpp:17-35`).
- `Enum` → Combo from `enum_labels` (finally giving enum labels a schema home).
- `Color3` → `ColorEdit3`; `Float3` → `DragFloat3`; `Bool` → Checkbox;
  `String` → InputText.
- `doc` → tooltip on hover; `units` folded into the format string.
- Modified-from-baseline fields render amber with a right-click context menu:
  *Reset to default / Reset to world / Copy path* — the visual language the
  Part Workbench params panel already established (`part_workbench.cpp:890-938`).
- `RequiresReload` groups use the LOD-settings draft pattern (`ui.h:303-311`): edits
  accumulate in a draft copy, an "Apply & Reload World" button commits — the
  existing `set_streaming_lod_overrides` + `commands.reload()` flow generalized.

### 6.2 Panels

- **New "Tunables" panel** (`Ui::draw_tunables_panel`, split Begin/End idiom like
  `draw_console_panel` at `ui.cpp:571-573`): filter box + one `CollapsingHeader` per
  registered group, sorted by path. Every future tunable gets UI for free by
  registering; nobody hand-writes sliders again.
- **Existing panels migrate incrementally.** Lighting/volumetrics/POM sliders in
  `draw_debug_panel` (`ui.cpp:955-1002`) are deleted in Phase 1 and replaced by
  `draw_group` calls on the same structs — panels can still choose *where* a group
  appears; the Tunables panel is the catch-all, not a forced destination.
- Save UX: toolbar dirty indicator per committed scope; save-on-exit prompt for
  unsaved Project/World edits; User scope autosaves silently.

### 6.3 Free byproducts

- **FIFO:** a generic `set <group.path>.<field> <value>` command in the
  `MATTER_CMD_FIFO` channel (`viewer_commands.h`), via registry path lookup —
  today only `budget` has a bespoke command.
- **Issue reports:** `write_state_json` gains a dump of all non-baseline property
  values; shot replay can restore them. Today it captures exactly two tunables by
  hand (`main.cpp:1449-1451`).

## 7. ECS unification (Phase 3)

`scene::FieldDescriptor` becomes (or wraps) `props::Desc`: add `offset`,
`enum_labels`, `doc`. Then:

- The four+ `strcmp` ladders in `main.cpp:97-350` (`field_get_float`,
  `field_set_float`, int/uint/bool/float3/quat variants, ~250 lines) collapse into
  the registry's generic offset-based accessors. `FieldCommands`
  (`properties_panel.h:33-48`) shrinks to "fetch component copy / store component
  copy" per kind; per-field routing is schema-driven.
- `enum_options_for` (`properties_panel.cpp:92-99`) is deleted; RigidBody.type labels
  move into the descriptor table.
- `Quaternion` stays a scene-registry-only type for now (the Euler-edit widget at
  `properties_panel.cpp:351` is ECS-specific).

Entity property *persistence* is out of scope here — entities serialize through the
world script's `static entities` (`RawEntityRecipe`), a separate authoring problem.

## 8. Env var consolidation (opportunistic, Phase 2+)

`Desc::env` lets a group declare its env overrides in the schema; the registry
applies them at bind time (layer 5) with one shared parser, replacing the three
copy-pasted `env_u32`/`env_f32`/`env_flag` helpers. Migration is per-group and
optional — env vars read in function-local statics deep in the engine
(e.g. `cell.cpp:272`) can stay until their subsystem adopts a group. The win is that
migrated vars become *visible*: the Tunables panel shows the env-forced value and
its source instead of an invisible process-start override.

## 9. Script-defined properties (Phase 4 — sketch)

Concept split, to keep semantics honest:

- **`static params`** (exists): bake **inputs**. Canonicalized, hashed into the part's
  content address; changing one produces a different artifact. Edited via Part
  Workbench, persisted via pins. Unchanged.
- **`static props`** (new): runtime **tunables**. Declared with schema metadata:

```js
class Windmill extends Part {
  static params = { bladeCount: 4 };            // bake input — rebakes on change
  static props = {
    spinSpeed: { default: 1.2, min: 0, max: 10, doc: "Rotations per second" },
    creakVolume: { default: 0.3, min: 0, max: 1 },
  };
}
```

The loader builds a **dynamic Group** at world connect: `Desc[]` allocated from the
parsed schema, values stored in an engine-owned buffer (not offsets into a C++
struct — the `construct_default` slot builds the buffer from the declared defaults).
Everything downstream — editor panel, sparse persistence to the world props file,
FIFO, baselines — works unchanged because it already goes through the registry's
typed accessors. Scripts read values through a binding
(`this.props.spinSpeed` backed by a getter into the buffer), and the world file's
persisted overrides apply before first tick.

World scripts get the same via `static props` on the World class (grouped as
`world.props.*`), which also gives `world_definition_loader` a natural place to
validate them with its existing `reject_unknown_spec_keys` strictness.

The schema-less JSON diff editor in `part_workbench.cpp:869-942` is the precedent
for dynamically-shaped property UI; once dynamic groups exist it should be rebuilt
on them (a `DynamicGroup` whose schema is inferred from JSON value kinds), removing
the second independent property-editor implementation.

## 10. Phasing

**Phase 1 — core + first real consumer**
1. Lift `JsonValue`/parser/writer into `util/json_doc` (part_workbench switches to it; behavior-neutral).
2. `matter/props.h` core: Desc/Group/builder, Registry, typed accessors, baseline capture, JSON round-trip, atomic save.
3. `property_editor.cpp` generic renderer + Tunables panel + dirty/save UX.
4. Migrate `ViewerStats` tunables: `pixel_budget`, `lighting`, `volumetrics`,
   `tileset_pom` → registered groups (World scope for the three settings structs,
   User scope for pixel_budget). Delete the hand sliders at `ui.cpp:955-1002` and the
   duplicate binding at `ui.cpp:595`.
5. Persistence: User + World scope files live.

*Acceptance:* tune volumetrics on StreamMountain, quit, relaunch → values restored;
"Reset to world" returns to authored JS values; the world props file contains only
the edited fields; a world with no edits produces no file.

**Phase 2 — breadth**
- LOD/streaming config through `RequiresReload` draft flow (replacing the bespoke
  `LodSettingsState` plumbing), camera prefs (User scope), last-world/UI prefs.
- Generic FIFO `set`, issue-report property capture, env-layer support + first
  migrated env vars (the `vt_residency.cpp:306-337` budget block is the natural pilot).

**Phase 3 — ECS unification** (§7).

**Phase 4 — script-defined props** (§9), after the Phase-5/6 scripting concepts firm up.

## 11. Open questions

1. **Commit policy for override files.** ~~Open~~ **Decided (2026-07-31):**
   `projects/*/editor/*.json` are committed — tuning is shareable project data.
   `editor_settings.json` (User scope) stays gitignored.
2. **Write-back into world JS.** Should "promote to authored" exist — a button that
   moves a world-scope override into the `.js` `static` block via `// @matter-data`
   markers (per the 2026-07-17 spec, with `write_lods_to_source`'s
   parse-verify/backup/restore contract)? Deferred; sidecar files make it unnecessary
   for v1 but authored-source consolidation may be wanted once values stabilize.
3. **String/Color in v1.** `Color3` is cheap and immediately useful (fog/sun color);
   `String` can wait unless a Phase-1 group needs it.
4. **Session-scope groups in the Tunables panel.** Debug toggles (`debug_view_mode`,
   overlay checkboxes) could register as `Session` scope for the free UI without
   persistence — worth doing opportunistically during migration.
