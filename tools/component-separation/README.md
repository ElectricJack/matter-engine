# Component dependency and ownership checks

`check_dependencies.py` checks that every procedural component stands on its own:

- geometry builds without the JavaScript host or the renderer;
- graphics builds without geometry or terrain;
- the neutral contracts depend on nothing.

It also checks that every directory has exactly one owner and that no algorithm is copied across a component boundary.

The tool reads `rules.json`, which lists the components, their allowed dependencies and audited exceptions. It does not edit any build file, CMake preset or workflow. CI consumes it as a command plus a JSON report.

```bash
python3 tools/component-separation/check_dependencies.py --list           # components, statuses, variants
python3 tools/component-separation/check_dependencies.py --checks static,duplicates
python3 tools/component-separation/check_dependencies.py --checks configure --component geometry
python3 tools/component-separation/check_dependencies.py --report build/component-deps/report.json
```

Windows (native PowerShell): use the supported MSVC toolchain, the same resolver and `VsDevCmd` environment as `tools/build-windows.ps1`, with one compile job:

```powershell
tools/component-separation/check-windows.ps1 -Checks static,duplicates,configure -Report build/component-deps/windows.json
```

From WSL, call the same script through `powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/component-separation/check-windows.ps1 ...`.

## Checks

| `--checks` | Needs | What it proves |
|---|---|---|
| `static` | Python only | **Module ownership**: component directories don't overlap, implemented native components have a `CMakeLists.txt`, scaffolds hold no code, absent components have no files, and `rules.json` agrees with `libs/MatterContracts/component-ownership.json`. **Public includes**: the transitive include closure of every public header stays inside its own component's public roots and allowed components, and reaches no forbidden header. **JS imports**: JavaScript components import nothing host-specific (`node:`, `fs`, …), no relative import leaves the package, and every bare specifier is declared. |
| `duplicates` | Python only | No file is copied verbatim, and no normalised window of code (default 8 lines) is shared, between two components or between a component and legacy in-place code. |
| `configure` | cmake, a C++ compiler | Each component is configured **alone** in a throwaway probe project (`add_subdirectory` of only that component), and the CMake File API reply is audited. The audit covers the targets that exist (`component-off`), the transitive link graph (`link`), every compiled source (`sources`), every include directory and whether it is PUBLIC (`include-dirs`), every CMake file read at configure time (`configure-inputs`), and sources in the component that nothing compiles (`unbuilt-sources`). Across all probes, `repository/-/compiled-twice` flags a source compiled by two components. |
| `build` | + Ninja | Builds the probe: one executable that includes every public header and links every object of the component, so undefined cross-component symbols fail the link. It then audits the compiler-reported header dependencies (`ninja -t deps`) and the archive symbols (`nm`, or `link /dump` with MSVC) against the forbidden symbol groups. `build` implies `configure`. |

The default is all four checks. Probes run one at a time, and `--jobs` (default 1) bounds the compile parallelism inside each one, so the tool never launches a full-box build. Probe trees live under `build/component-deps/<platform>/` and are incremental; `--fresh` deletes them first.

### Results

Each result is keyed `component/variant/check`. Static results use the variant `-`; probe results use the variant name (`default`, `vulkan`, …). A result is one of:

- **PASS**: checked, no violations.
- **FAIL**: at least one violation. Each violation has a stable `code` (for example `link.forbidden-component` or `public-include.forbidden`) and a human-readable detail.
- **UNTESTED**: not checked, always with the reason.

UNTESTED covers:

- `scaffold` and `absent` components, with the rules' reason for why they are not built yet;
- variants whose capability is missing on this machine (for example `graphics/vulkan` without a Vulkan SDK);
- checks downstream of a failed configure or build.

An UNTESTED result is never counted as a pass. The summary lists it separately.

### Exit codes

| Code | Meaning |
|---|---|
| 0 | No violations. UNTESTED results are listed but allowed. |
| 1 | At least one violation. Also returned when `--strict-untested` was given and anything is UNTESTED, or when a `--require-capability` capability is unavailable (reported as `capability.unavailable`). |
| 2 | Usage, rules or environment error. Covers an unknown check, component or capability; a malformed `rules.json`; an audited exception without a reason; and a missing cmake. No report is written. |

Use `--require-capability NAME` where a capability must be present. `check-windows.ps1` passes `vulkan-sdk`, because the supported Windows toolchain installs it. An unknown capability name is an error, never ignored.

`--host-cache KEY=VALUE` (repeatable) adds a probe cache entry that locates a host tool, for example a Vulkan SDK outside the default search path:

```bash
mkdir -p /tmp/vulkan-sdk && cp -r third_party/Vulkan-Headers/include /tmp/vulkan-sdk/
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
python3 tools/component-separation/check_dependencies.py --checks configure,build --component graphics \
  --host-cache Vulkan_INCLUDE_DIR=/tmp/vulkan-sdk/include \
  --host-cache Vulkan_LIBRARY=/usr/lib/x86_64-linux-gnu/libvulkan.so.1 \
  --host-cache MATTER_GRAPHICS_GLSLC=/path/to/glslc --require-capability vulkan-sdk
```

The headers are copied out of the repository because the variant allows Vulkan include directories only as external SDK paths. Pointed at `third_party/Vulkan-Headers/include` in place, the probe reports it as a repository directory that consumers inherit (`include-dir.public-leak`). The probe build directory keeps its CMake cache, so delete `build/component-deps/<platform>/graphics-vulkan` before a run that should not reuse earlier host entries.

A host entry may not override a key the rules set (`probe_cache` or a variant's `cache`); that is a rules error (exit 2), so a host flag cannot switch a component back on.

## Rules (`rules.json`)

The schema is `matter.component-dependency-rules.v1`. Loading is strict: an unknown key is an error, so a misspelt allowance cannot silently widen anything.

**Components.** Each component has a `status`:

- `implemented`: checked.
- `scaffold`: a directory with a build file but no code yet. UNTESTED, and it must stay empty of code.
- `absent`: not created yet. UNTESTED, and its directory must not exist.

A non-implemented component must state its `reason`.

Each component also has a `kind` (`native`, `javascript`, `browser`, `adapter`), a `directory`, `targets` and `allowed_components`. An implemented component may only be allowed implemented components.

**Variants.** These are extra probe configurations. A variant sets cache entries and may add targets, sources and public headers. A variant can be gated on a declared `capability` with `requires_capability`, and its `unavailable_patterns` recognise a configure failure caused by that missing capability.

**Audited exceptions.** These tolerate today's in-place legacy code during the extraction. Every entry is `{"path": "<file, directory or glob>", "reason": "<why>"}`, and a missing or empty reason is a rules error. The exception lists are:

- `legacy_sources`
- `legacy_include_dirs`
- `legacy_headers`
- `unbuilt_sources`
- `configure_inputs`
- `private_include_roots`

Every tolerated item is listed in the passing result's notes (shown with `--verbose`), so the debt stays visible. Remove an entry once its code moves.

The legacy lists are audited before the foreign-owner check, so a component that still compiles or includes another component's file in place (named `BOUNDARY DEBT` in its reason) is reported as audited debt, not as an allowed dependency. `allowed_components` is never widened to make such a build pass.

**Monolith sources (`monolith_sources`).** A component source the probe does not compile because the monolith still does (for example `graphics`' Vulkan scene renderer) may be listed here. The entry is honoured only when `cmake/manifests/*.sources` actually compiles that file, directly or through a listed forwarding entrypoint that `#include`s it (`depcheck/monolith.py`). Otherwise `unbuilt-sources` fails with `source.monolith-unlisted`: a stale manifest can neither silently keep relinking the monolith copy nor silently drop it.

**Variant sources and headers.** A variant's `sources` (relative to the component directory) are excused from `unbuilt-sources` in the other variants, and a variant's `public_headers` are left out of the other variants' probe, so an optional module (`asset_pipeline/products`) is proven only in the configuration that builds it.

**Forbidden lists.** These are `forbidden_public_includes`, `forbidden_external_headers` and `forbidden_symbols`. `forbidden_symbols` names entries in the top-level `symbol_groups`.

**JavaScript (`js`).** The settings are `include`/`exclude` globs, `forbidden_specifiers` (regular expressions) and `allowed_bare_specifiers`. A relative import that leaves the package is allowed only when it lands in a component listed in `allowed_components` (noted as `imports allowed component …`); anything else fails `js-import.escapes-package`.

### Current repository state

Every component is `implemented` and checked (`--list`):

- `contracts`, `geometry`, `terrain`, `build`, `host_quickjs`, `geometry_c`
- `graphics`, with a `vulkan` variant that needs `vulkan-sdk`
- `asset_pipeline`, with `minimal` (all optional modules off) and `products` (animation and collision modules on) variants
- `authoring` (JavaScript), `web_procedural` (browser)
- `component_adapters`

A new component starts as `scaffold` or `absent`; flip it to `implemented` when it is extracted. It is then checked automatically, and the checks fail until its own rules are complete.

## CI

`tools/ci-component-matrix.py` is the CI entrypoint (the `component-matrix` job in `.github/workflows/tests.yml`). It configures, builds and runs CTest for each component configuration from its own directory, with nothing else in the build:

| Row | Source directory |
|---|---|
| `contracts` | `libs/MatterContracts` |
| `geometry-headless` | `libs/ProceduralGeometry` |
| `graphics-only` | `libs/MatterGraphics` |
| `terrain-only` | `libs/TerrainGeneration` |
| `build` | `libs/ProceduralBuild` |
| `asset-pipeline` | `libs/AssetPipeline` |
| `geometry-c` | `bindings/geometry-c` |
| `host-quickjs` | `MatterEngine3/src/script/portable_host` |
| `authoring-adapter` | `MatterEngine3/src/component_adapters/authoring` |
| `terrain-coordinator` | `MatterEngine3/src/component_adapters/terrain_coordinator` |

The gates then run in this order:

1. `source-manifests`: `tools/check-source-manifests.py`.
2. `dependency-rules`: this checker with `static,duplicates,configure`.
3. `dependency-fixtures`: the fixture suite below.
4. `wasm`: `bindings/geometry-c/wasm/build-smoke.py`. This WASM conformance gate uses the native `geometry-c` consumer as its oracle.

The WASM gate needs an Emscripten SDK (`--emsdk` or `EMSDK`). Without one it is NOT RUN, which is never a pass. `--require wasm` turns NOT RUN into a failure. Every row writes a log, and `summary.json` (`matter.component-matrix-run.v1`) lists each row's commands, exit codes and seconds, plus `not_run` and `failed`. Any failed row exits 1.

```bash
python3 tools/ci-component-matrix.py --jobs 4                                # hosted CI: wasm NOT RUN
python3 tools/ci-component-matrix.py --jobs 2 --emsdk ~/emsdk --require wasm  # full local run
python3 tools/ci-component-matrix.py --only asset-pipeline --only dependency-rules
```

The full native editor and package stay with `tools/ci-windows.ps1`. Wall times are recorded as evidence only and never decide pass or fail.

A focused job can also run the checker alone. Run the command, keep the report, and gate on the exit code:

```bash
python3 tools/component-separation/check_dependencies.py --checks static,duplicates --report out/component-deps-static.json
python3 tools/component-separation/check_dependencies.py --checks configure --report out/component-deps-configure.json
```

The report (`matter.component-dependency-report.v1`) contains:

- `summary`: `{pass, fail, untested, violations}`.
- `results[]`: `{component, variant, check, status, violations[{code, detail}], reason?, notes?}`.
- A flat `violations[]` list: `{check, code, detail}`.
- `untested[]`: `{check, reason}`, so a job summary can list what was not proven.
- Run context: the platform, the checks, the components, the toolchain and the elapsed time.

Measured with one compile job:

| Command | WSL (GCC) | Windows (`check-windows.ps1`, MSVC) |
|---|---|---|
| `static,duplicates` | ≈46 s | — |
| `static,duplicates,configure` | ≈68 s | ≈40 s |
| `build` (implies `configure`) | ≈757 s cold; ≈85 s incremental, ≈290 s when the probe project itself changed | ≈132 s cold |

The configure check is the cheap gate. The build check suits a nightly or path-filtered job.

## Fixture tests

`tests/component-separation/test_dependency_checks.py` runs the checker against a miniature repository (`tests/component-separation/dependency-fixtures/base`). Each case adds one deliberate violation, and the test asserts the exact violation codes and failing results. It also asserts that every violation code the checker can emit is exercised by some case. Cases that need cmake, Ninja and a compiler are skipped, with the reason, when the toolchain is missing.

```bash
python3 tests/component-separation/test_dependency_checks.py                  # no pytest needed
python3 -m pytest tests/component-separation/test_dependency_checks.py -q
python3 -m pytest tests/component-separation/test_dependency_checks.py -q -k public_header   # one family
MATTER_DEPCHECK_KEEP_FIXTURES=1 python3 -m pytest ... -k probe_raw_library -s              # keep the tree
```

On Windows, `tools/component-separation/check-windows.ps1 -SelfTest` runs the same suite with the supported MSVC toolchain. It also covers the `link /dump` symbol scan and the MSVC header dependencies.

The suite runs serially with one compile job per probe: ≈110 s on WSL and ≈150 s on Windows. Do not run it in parallel with other suites, and do not run the Windows and WSL suites at the same time.

## Layout

- `check_dependencies.py`: the command line and orchestration.
- `depcheck/rules.py`: strict rules loading and validation.
- `depcheck/static_checks.py`: ownership, the public include closure and JS imports.
- `depcheck/duplicates.py`: verbatim and windowed duplicate detection.
- `depcheck/cmake_probe.py`: probe generation, the CMake File API audit, Ninja header deps and symbol scans.
- `depcheck/report.py`, `depcheck/text.py`: results, the report and comment-stripping helpers.
- `check-windows.ps1`: the supported-toolchain wrapper for Windows.
