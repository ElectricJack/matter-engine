# Native editor registration census

The existing `editor_registration_census` CTest is native-MSVC-only by
default. It launches the MSVC editor with `MATTER_REGISTRATION_CENSUS=1`,
compares its four live registration sets with
`editor_registration_expected.json`, and repeats the comparison with an
unregistered byte-string decoy appended to a copy of that same executable.
The editor exits before the first world bake. Neither executable strings nor
the observed census can create or update the expected manifest.

## Reviewed manifest origins

The initial manifest was reviewed against the working source on 2026-08-30,
after the completed render-eligibility and character integration work. The
native log at `MatterEditor/build/baselines/msvc/registration-census/msvc-runtime.log`
was corroborating evidence, not the authority for accepting its names.

| Set | Source of the reviewed names |
| --- | --- |
| world (73) | `MatterEditor/src/ui.cpp::scan_worlds` and `MatterEngine3/include/matter/project_layout.h::scene_scripts`: the 72 checked-in matching `projects/world_demo/scenes/**/<Name>/<Name>.js` scripts plus `projects/primitive_demo/worlds/Primitives.js`. Group folders are traversed recursively; object/helper directories and descendants of a recognized scene are excluded. |
| dsl (107) | The executed `bind(...)` calls in `MatterEngine3/src/dsl_bindings.cpp::install_bindings` and `MatterEngine3/src/pf_bindings.cpp::install_pf_bindings`. The census measures newly installed QuickJS globals, so the `Math.random` member replacement is not a new global. The commented-out `__terrainVolume` binding is intentionally absent; `__terrainVolumeTiled`, `__dsl_rayTraced`, `__dsl_solidSource`, and `__dsl_surfaceVertex` are present. |
| property (22) | The bindings executed by `MatterEditor/src/editor_props.cpp::EditorProps::init`, including the local `s_gi` schema bound as `render.gi`. Most schemas are local; `ui.h` supplies the two viewer status groups, `streaming_lod_prefs.h` supplies `stream.lod`, and `matter/stream_settings.h` plus `matter/vt_budgets.h` supply `stream.runtime`, `vt.residency`, and `vt.enrich`. World-specific dynamic groups are not yet installed at this startup seam. |
| editor (53) | The live `must_register_handler<T>` handles before the census return in `MatterEditor/src/main.cpp`, mapped to their `MT_COMMAND_NAME` declarations in `MatterEditor/src/viewer_commands.h`. This includes the accepted `fifo.character` handler, the three ticketed `agent.*` metadata commands, the six typed `selection.*` commands, the three `viewport.*` commands (`pick`, `pick_select`, `capture`), `view.focus`, `view.set_camera`, and `render.reset_temporal`. |

The four sets must match exactly, with ordinal/case-sensitive names. Missing
and unexpected registrations are both errors. Empty/non-array categories,
duplicate or non-string identifiers, unexpected categories, malformed JSON,
and zero/multiple census records are rejected.

The four sets were refreshed on 2026-10-11 against checked-in source
`35289874a52630a6439353666ed26179dd334ab2`. Each added world name has its
matching `class <Name> extends World` declaration and is discoverable through
the live scanner's rules. The two added DSL names are bound by `install_bindings`
and wrapped by `Part.solidSource` and `Part.surfaceVertex` in `part_base.js.h`.
The added property is the `s_gi` schema bound by `EditorProps::init`; the added
editor commands are `ViewSetCamera` and `RenderResetTemporal`, both registered
before the census return. No existing name was removed. Runtime output was
only corroborating evidence.

For an intentional registration change, review the declaring source and the
live registry consumer, edit the manifest explicitly in that same change, and
run the native gate. Do not copy a runtime log into the manifest to make a
failure disappear. The result JSON records the manifest path and SHA256.

## Verification

From native PowerShell at the repository root:

```powershell
./cmake/tests/editor_registration_census.ps1
./cmake/tests/editor_registration_census_contract_tests.ps1
```

The contract test runs the real native gate with an explicitly nonexistent
MinGW path, proving the rollback editor is not required. It then exercises
the shared validator with independent literal fixtures: each category loses
a name, gains a name, changes case, duplicates a name, or supplies invalid
data. `-ValidatorOnly` runs those quick validator checks without an editor.
The native gate retains the PE-overlay decoy test: a string present in the
executable but absent from live registries must not appear in any set.

The historical migration parity check remains available only through the
explicit `-CompareMinGW` switch with a deliberately selected `-MinGWEditor`.
Normal CTest never enables it, resolves its default path, or launches it.
That opt-in also requires the same reviewed manifest, so an old rollback
binary cannot act as an oracle for current MSVC registrations. No rollback
build or launch is required to validate the native gate.
