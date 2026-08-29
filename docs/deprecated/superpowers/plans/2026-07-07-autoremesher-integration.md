# autoremesher Integration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an opt-in, per-part retopo stage that runs on the whole-part flatten mesh (between `part_flatten` and the QEM LOD ladder), backed by a vendored headless subset of huxingyi/autoremesher and a new shared `MeshIndexed` boundary type in MatterSurfaceLib.

**Architecture:** Vendored library at `Libraries/autoremesher_core/` exposing a single `autoremesher::remesh()` entrypoint over an indexed mesh. MatterSurfaceLib adds `MeshIndexed` + `MeshTransform` + `mesh_retopo` alongside the existing QEM simplifier. MatterEngine3 wires the schema opt-in through `dsl_bindings.cpp`, calls MSL from the flatten pipeline, and caches `.retopo.part` artifacts. QEM ladder and cluster split consume the retopo'd mesh unchanged.

**Tech Stack:** C++17, existing per-project Makefile build system, quickjs-ng DSL, raylib, per-`MatterSurfaceLib`-project test binaries (`main.cpp` runs test suites), vendored deps `geogram` (BSD-3), `isotropicremesher` (Instant-Meshes-derivative), `eigen` (MPL-2), `tbb` (Apache-2).

## Agent Model Assignment

Each task carries an **Agent Model:** designation identifying which model runs the task and, where applicable, which model reviews the work. Steps within a task use the same model as the task.

- **Opus** — investigation, judgment calls, open-ended debug loops, semantic-preserving refactors, integration decisions. Any task where the plan's code is aspirational or where discovering the existing pattern is part of the work.
- **Sonnet implementation, Opus review** — tasks where the code is fully specified in the plan and implementation is largely mechanical. Sonnet writes; Opus does a targeted review focused on the specific risk noted per task (not a full re-read).

Reviews after Sonnet tasks should be small, focused checks — not full task redos. If Opus review surfaces issues that require substantial rework, that's a signal the task was miscategorized and should be reclassified as Opus-implemented for the fix.

## Global Constraints

Copied verbatim from the spec — every task's requirements implicitly include these:

- **License**: `autoremesher_core` and every vendored thirdparty subtree must preserve upstream copyright notices on every cherry-picked file. Top-level `LICENSE` file in `Libraries/autoremesher_core/` copies upstream MIT verbatim.
- **No Qt**: zero Qt files land in `Libraries/autoremesher_core/`. Anything under `resources/`, `shaders/`, `*.ui`, `main.cpp`, `mainwindow.*`, or including `<Q...>` is excluded.
- **No CGAL / no GPL modules**: `thirdparty/geogram/` includes only the BSD-3 core (no `tetgen`, no GPL-tagged modules).
- **Determinism contract**: `MSL::retopo()` and `autoremesher::remesh()` must produce byte-identical output given `(input, options)`. `RetopoOptions.threads = 1` is the default. Same for `autoremesher::Options.threads = 1`.
- **No subprocess isolation**: the library is linked in-process. Crashes are bugs to fix, not to isolate around.
- **MSL is opened for this feature** (per-feature scope decision, 2026-07-07). New MSL files are permitted where the spec names them.
- **Windows cross-build**: after any engine/library change, run `make windows` per the "Always rebuild the Windows binary" project memory. Not a hard-fail in every task, but any task touching MSL or MatterEngine3 must not break the Windows target.
- **Platform triple in cache key**: `.retopo.part` cache keys embed the platform triple (linux-x86_64 vs windows-cross-x86_64) to sidestep cross-platform FP determinism ambiguity.
- **Existing worlds must bake identically**: retopo defaults to `enabled: false`. No schema in `MatterEngine3/examples/world_demo/schemas/` may be modified as part of this plan's tasks beyond adding an opt-in retopo block on Tree in the final smoke task.

---

## File Structure

**New files (Libraries/autoremesher_core/):**
- `LICENSE` — MIT text from upstream, verbatim
- `UPSTREAM.md` — pinned commit, file list, extraction rationale
- `Makefile` — builds `libautoremesher_core.a`
- `include/autoremesher/remesh.h` — sole public header
- `src/remesh.cpp` — driver composing pipeline stages
- `src/mesh_sanitize.cpp` — dedup + non-manifold cleanup (from upstream)
- `src/param_hdc.cpp` — HDC parameterization driver (from upstream)
- `src/quad_extract.cpp` — quad-dominant extraction (from upstream)
- `src/quad_to_tri.cpp` — triangulate quads for output (from upstream)
- `thirdparty/geogram/`, `thirdparty/isotropicremesher/`, `thirdparty/eigen/`, `thirdparty/tbb/` — vendored subtrees

**New files (MatterSurfaceLib/):**
- `include/mesh_indexed.hpp` — `MeshIndexed`, `WeldOptions`, `from_tri`, `to_tri`
- `include/mesh_transform.hpp` — `reproject_triex` shared helper
- `include/mesh_retopo.hpp` — `RetopoOptions`, `RetopoResult`, `retopo`
- `src/mesh_indexed.cpp` — weld/unweld implementation
- `src/mesh_transform.cpp` — reprojection implementation
- `src/mesh_retopo.cpp` — MSL wrapper over `autoremesher_core`
- `tests/mesh_indexed_tests.cpp` — TDD tests
- `tests/mesh_retopo_tests.cpp` — TDD tests

**Modified (MatterSurfaceLib/):**
- `include/mesh_simplifier.hpp` — add `MeshIndexed` overload
- `src/mesh_simplifier.cpp` — implement `MeshIndexed` overload as a shim over the existing `raylib::Mesh` path
- `Makefile` — link `autoremesher_core`; compile new .cpp files; include new tests in `main.cpp`
- `main.cpp` — register new test suites

**Modified (MatterEngine3/):**
- `src/lod_bake.cpp` — route through `MeshIndexed`; remove double round-trip; use MSL's `reproject_triex`
- `include/lod_bake.h` — remove `reproject_triex` declaration (moved to MSL)
- `src/part_flatten.cpp` — insert retopo hook + cache write/read after flatten
- `src/dsl_bindings.cpp` — parse `retopo` block on part definitions
- `include/part_asset_v2.h` — add `RetopoSettings` to part metadata
- `src/part_asset_v2.cpp` — serialize `RetopoSettings` into the part cache artifact
- `Makefile` — no change if MSL Makefile already exposes the new headers; verify

**New tests (MatterEngine3/):**
- `tests/retopo_integration_tests.cpp` — bake-pipeline integration test

---

## Task Order and Dependency Graph

```
1  scaffold + investigate upstream         → UPSTREAM.md
2  vendor thirdparty deps                  → needs 1
3  vendor autoremesher pipeline sources    → needs 1, 2
4  Libraries/autoremesher_core/Makefile    → needs 3
5  autoremesher/remesh.h public API        → parallel with 3, 4
6  src/remesh.cpp driver + smoke test      → needs 4, 5

7  MSL: MeshIndexed type + tests           → parallel with 1–6
8  MSL: mesh_transform + reproject_triex   → needs 7
9  MSL: simplifier MeshIndexed overload    → needs 7
10 MSL: mesh_retopo + tests                → needs 6, 7, 8

11 Engine: lod_bake refactor to MeshIndexed→ needs 8, 9
12 Engine: DSL retopo block + RetopoSettings→ parallel with 11
13 Engine: part_flatten hook + cache       → needs 10, 11, 12
14 Engine: retopo integration test         → needs 13
15 E2E: Tree.js retopo opt-in + smoke      → needs 14
```

Tasks 7–10 can proceed in parallel with 1–6 — MSL work only needs the vendored library's public header, which is written in Task 5 (a header-only task with no library-build dependency). If executed strictly serially, order is 1→2→3→4→5→6→7→8→9→10→11→12→13→14→15.

---

## Task 1: Scaffold `Libraries/autoremesher_core/` and produce UPSTREAM.md

**Agent Model:** Opus — upstream repo enumeration and UPSTREAM.md drafting require judgment about which upstream files map to the spec's named pipeline stages, and whether Deviations need to be recorded.

**Files:**
- Create: `Libraries/autoremesher_core/LICENSE`
- Create: `Libraries/autoremesher_core/UPSTREAM.md`
- Create: `Libraries/autoremesher_core/include/autoremesher/.gitkeep`
- Create: `Libraries/autoremesher_core/src/.gitkeep`
- Create: `Libraries/autoremesher_core/thirdparty/.gitkeep`

**Interfaces:**
- Consumes: nothing
- Produces: `UPSTREAM.md` is the source of truth used by Tasks 2 and 3 to know exactly which upstream commits and files to vendor. Every path referenced by later tasks must appear here.

- [ ] **Step 1: Enumerate upstream repo at HEAD**

Run: `gh api repos/huxingyi/autoremesher/commits/master --jq '.sha' | head -c 40`

Record the resulting SHA. Then run:

```bash
gh api repos/huxingyi/autoremesher/contents/src --jq '.[].name' | sort
gh api repos/huxingyi/autoremesher/contents/include --jq '.[].name' | sort
gh api repos/huxingyi/autoremesher/contents/thirdparty --jq '.[].name' | sort
```

Save the outputs — they populate the file list in UPSTREAM.md below.

- [ ] **Step 2: Fetch upstream LICENSE verbatim**

Run: `gh api repos/huxingyi/autoremesher/contents/LICENSE --jq '.content' | base64 -d > /tmp/upstream_license.txt`

Verify the first line contains "MIT License". Then copy to the vendored location:

```bash
cp /tmp/upstream_license.txt "Libraries/autoremesher_core/LICENSE"
```

- [ ] **Step 3: Draft UPSTREAM.md**

Write `Libraries/autoremesher_core/UPSTREAM.md` with the structure below. Fill in the exact SHA from Step 1 and the file lists from Step 1.

```markdown
# autoremesher_core — vendoring notes

**Upstream**: https://github.com/huxingyi/autoremesher
**Upstream license**: MIT (see LICENSE)
**Pinned commit**: <SHA from Step 1>
**Pinned date**: 2026-07-07

## What's vendored

Cherry-picked pipeline files (see extraction rules below). None of upstream's
Qt UI, main entry, or resource files are pulled in.

### Pipeline sources (from upstream `src/`)

<For each file below, use the exact upstream filename. If upstream's structure
does not match the four files enumerated in the spec (mesh_sanitize, param_hdc,
quad_extract, quad_to_tri), use the actual upstream filenames and note in the
"Deviations" section below.>

- `<upstream/src/foo.cpp>` → `src/mesh_sanitize.cpp`
- `<upstream/src/bar.cpp>` → `src/param_hdc.cpp`
- `<upstream/src/baz.cpp>` → `src/quad_extract.cpp`
- `<upstream/src/qux.cpp>` → `src/quad_to_tri.cpp`

Companion headers (from upstream `include/` or inline in `src/`) come across
alongside their .cpp counterparts, same directory in our tree.

### Thirdparty subtrees (from upstream `thirdparty/`)

- `thirdparty/geogram/` — BSD-3 core only, no tetgen wrapper or any GPL-tagged
  module. Pinned SHA: <capture from `gh api` in Step 1>
- `thirdparty/isotropicremesher/` — pinned SHA: <capture>
- `thirdparty/eigen/` — MPL-2, headers only. Pinned SHA: <capture>
- `thirdparty/tbb/` — Apache-2. Pinned SHA: <capture>

## Extraction rules

- Every synced file preserves its upstream copyright header verbatim.
- Zero Qt files. Anything under `resources/`, `shaders/`, `*.ui`, `main.cpp`,
  `mainwindow.*`, or including `<Q...>` is excluded.
- Non-pipeline utilities (upstream logging, Qt argument parsers, GUI helpers)
  are excluded. The pipeline call graph is: sanitize → parameterize →
  quad-extract → triangulate.
- Every future sync is a deliberate cherry-pick against an updated pinned SHA,
  not a `git subtree pull`.

## Deviations from spec

<If upstream structure doesn't match the spec's exact file names, note the
mapping here — e.g. "spec says 'param_hdc.cpp' but upstream calls it
'HalfEdgeDirectionalCoverage.cpp'; we renamed on import." If none, write "None."">
```

- [ ] **Step 4: Create empty directory tree with .gitkeep sentinels**

```bash
mkdir -p "Libraries/autoremesher_core/include/autoremesher"
mkdir -p "Libraries/autoremesher_core/src"
mkdir -p "Libraries/autoremesher_core/thirdparty"
touch "Libraries/autoremesher_core/include/autoremesher/.gitkeep"
touch "Libraries/autoremesher_core/src/.gitkeep"
touch "Libraries/autoremesher_core/thirdparty/.gitkeep"
```

- [ ] **Step 5: Verify scaffolding**

Run:

```bash
test -f "Libraries/autoremesher_core/LICENSE" && \
test -f "Libraries/autoremesher_core/UPSTREAM.md" && \
test -d "Libraries/autoremesher_core/include/autoremesher" && \
test -d "Libraries/autoremesher_core/src" && \
test -d "Libraries/autoremesher_core/thirdparty" && \
echo OK
```

Expected: `OK`.

Then verify UPSTREAM.md has an actual SHA (not the literal string `<SHA from Step 1>`):

```bash
grep -E "Pinned commit.*[a-f0-9]{40}" "Libraries/autoremesher_core/UPSTREAM.md"
```

Expected: one match line containing a real 40-char hex SHA.

- [ ] **Step 6: Commit**

```bash
git add "Libraries/autoremesher_core/"
git commit -m "chore(autoremesher_core): scaffold vendored library dir with UPSTREAM.md"
```

---

## Task 2: Vendor thirdparty deps into `Libraries/autoremesher_core/thirdparty/`

**Agent Model:** Sonnet implementation, Opus review — mechanical `curl | tar` per subtree and known-name GPL-module removal. Opus review confirms no accidental Qt content survived, GPL modules are absent, and sampled copyright headers were preserved.

**Files:**
- Create: `Libraries/autoremesher_core/thirdparty/geogram/`
- Create: `Libraries/autoremesher_core/thirdparty/isotropicremesher/`
- Create: `Libraries/autoremesher_core/thirdparty/eigen/`
- Create: `Libraries/autoremesher_core/thirdparty/tbb/`

**Interfaces:**
- Consumes: `Libraries/autoremesher_core/UPSTREAM.md` from Task 1 for pinned SHAs
- Produces: vendored source trees under `thirdparty/`. Task 4 (Makefile) references specific paths under these directories.

- [ ] **Step 1: Fetch each thirdparty subtree tarball at its pinned commit**

For each of `geogram`, `isotropicremesher`, `eigen`, `tbb`, download a tarball of the exact commit pinned in UPSTREAM.md. Example for geogram (repeat for each; look up the upstream repo URL from `autoremesher`'s `thirdparty/<name>/README.md` or from the upstream `autoremesher.pro` file):

```bash
GEOGRAM_URL="https://github.com/BrunoLevy/geogram"
GEOGRAM_SHA="<from UPSTREAM.md>"
curl -L "$GEOGRAM_URL/archive/$GEOGRAM_SHA.tar.gz" -o /tmp/geogram.tar.gz
mkdir -p "Libraries/autoremesher_core/thirdparty/geogram"
tar -xzf /tmp/geogram.tar.gz -C "Libraries/autoremesher_core/thirdparty/geogram" --strip-components=1
```

Repeat with the correct URL/SHA for each of the other three.

- [ ] **Step 2: Remove GPL-tagged geogram modules**

Geogram's tree contains optional GPL modules that must not be pulled in.
Enumerate them:

```bash
find "Libraries/autoremesher_core/thirdparty/geogram" -type d -name tetgen -o -name mesh_CSG_operations -o -name mesh_repair_GPL
```

For every directory the find command returns, remove it:

```bash
find "Libraries/autoremesher_core/thirdparty/geogram" -type d \
  \( -name tetgen -o -name mesh_CSG_operations -o -name mesh_repair_GPL \) \
  -exec rm -rf {} +
```

- [ ] **Step 3: Scan for accidental Qt content in each subtree**

Run:

```bash
grep -rlE '^#include *<Q[A-Z]' "Libraries/autoremesher_core/thirdparty/" || echo "no Qt includes found"
find "Libraries/autoremesher_core/thirdparty/" -name '*.ui' -o -name '*.qrc' | head
```

If either surfaces content, remove those files. Expected on completion:
- The grep prints `no Qt includes found`.
- The find prints nothing.

- [ ] **Step 4: Verify copyright headers preserved**

Sample five random `.cpp` and `.h` files across the four subtrees and confirm each carries an upstream copyright block:

```bash
for f in $(find "Libraries/autoremesher_core/thirdparty/" -name '*.cpp' -o -name '*.h' | shuf | head -5); do
  echo "=== $f ==="
  head -20 "$f"
done
```

Every sampled file should start with a copyright/license header. If any doesn't, stop and investigate — either the subtree extraction was incomplete or upstream shipped files without headers (unusual).

- [ ] **Step 5: Commit**

```bash
git add "Libraries/autoremesher_core/thirdparty/"
git commit -m "chore(autoremesher_core): vendor geogram, isotropicremesher, eigen, tbb"
```

---

## Task 3: Vendor autoremesher pipeline sources

**Agent Model:** Opus — upstream filenames rarely match the spec's naming verbatim; requires deciding on file-to-file renames, sourcing companion headers, updating UPSTREAM.md's Deviations, and stripping Qt includes without breaking compilation.

**Files:**
- Create: `Libraries/autoremesher_core/src/mesh_sanitize.cpp` (+ companion header if upstream has one)
- Create: `Libraries/autoremesher_core/src/param_hdc.cpp` (+ companion header if upstream has one)
- Create: `Libraries/autoremesher_core/src/quad_extract.cpp` (+ companion header if upstream has one)
- Create: `Libraries/autoremesher_core/src/quad_to_tri.cpp` (+ companion header if upstream has one)

**Interfaces:**
- Consumes: `UPSTREAM.md` file list from Task 1
- Produces: the pipeline sources that Task 6 (`remesh.cpp`) will compose. If the upstream filenames differ from the spec's names (which UPSTREAM.md may have noted as a Deviation), Task 6's `#include`s must be updated to match.

- [ ] **Step 1: Copy each pipeline file from upstream via `gh api`**

For each of the four upstream pipeline files enumerated in `UPSTREAM.md`, download and place at the mapped destination. Example (repeat for each):

```bash
gh api repos/huxingyi/autoremesher/contents/src/<upstream_filename>.cpp?ref=<pinned_sha> \
  --jq '.content' | base64 -d > "Libraries/autoremesher_core/src/mesh_sanitize.cpp"
```

Do the same for any `.h` companions upstream ships alongside these sources.

- [ ] **Step 2: Strip Qt headers**

Autoremesher's non-UI pipeline files should not include Qt, but occasionally reference `<QtCore>` for containers. Remove any Qt includes:

```bash
sed -i '/^#include *<Q[A-Z]/d' "Libraries/autoremesher_core/src/"*.cpp
```

If a stripped include leaves a compile error (e.g., `QVector` used in the body), replace the usage with `std::vector` and record the substitution as a comment in the file's header block. Do NOT introduce Qt as a dependency here.

- [ ] **Step 3: Verify copyright headers preserved**

Every file placed in Step 1 should carry the upstream copyright. Check:

```bash
for f in "Libraries/autoremesher_core/src/"*.cpp; do
  head -5 "$f"
  echo "---"
done
```

Every file should show a copyright block. If any doesn't, fetch that file's original header text from upstream and prepend it.

- [ ] **Step 4: Commit**

```bash
git add "Libraries/autoremesher_core/src/"
git commit -m "chore(autoremesher_core): vendor pipeline sources (sanitize/param/extract/triangulate)"
```

---

## Task 4: `Libraries/autoremesher_core/Makefile` builds `libautoremesher_core.a`

**Agent Model:** Opus — the iterative "add source until it links" loop on geogram is open-ended; may need to fall back to a CMake shim per the spec's noted open item. Requires reading geogram's headers to walk the include graph.

**Files:**
- Create: `Libraries/autoremesher_core/Makefile`

**Interfaces:**
- Consumes: sources vendored in Tasks 2 and 3.
- Produces: static library `Libraries/autoremesher_core/libautoremesher_core.a` and headers at `Libraries/autoremesher_core/include/autoremesher/`. Task 6 links this. MSL's Makefile in Task 10 also links this.

- [ ] **Step 1: Write the Makefile**

The exact contents of Makefile depend on which geogram sources compile as part of the library and whether tbb needs its own build shim. Start with the following skeleton and add sources/flags iteratively as the build reveals what's needed:

```makefile
# Libraries/autoremesher_core/Makefile
# Builds libautoremesher_core.a — the headless subset of huxingyi/autoremesher.

CXX ?= g++
AR  ?= ar

CXXFLAGS := -std=c++17 -O2 -fPIC -Wall -Wno-unused -Wno-deprecated
CXXFLAGS += -Iinclude
CXXFLAGS += -Ithirdparty/eigen
CXXFLAGS += -Ithirdparty/geogram/src/lib
CXXFLAGS += -Ithirdparty/isotropicremesher
CXXFLAGS += -Ithirdparty/tbb/include

# Pipeline sources (cherry-picked from upstream).
PIPELINE_SRC := \
    src/remesh.cpp \
    src/mesh_sanitize.cpp \
    src/param_hdc.cpp \
    src/quad_extract.cpp \
    src/quad_to_tri.cpp

# Thirdparty sources. These lists are populated in Step 2 below as the build
# reveals which specific translation units are needed by the pipeline call graph.
GEOGRAM_SRC :=
ISOTROPICREMESHER_SRC :=
TBB_SRC :=

ALL_SRC := $(PIPELINE_SRC) $(GEOGRAM_SRC) $(ISOTROPICREMESHER_SRC) $(TBB_SRC)
ALL_OBJ := $(ALL_SRC:.cpp=.o)

libautoremesher_core.a: $(ALL_OBJ)
	$(AR) rcs $@ $^

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	find . -name '*.o' -delete
	rm -f libautoremesher_core.a

.PHONY: clean
```

- [ ] **Step 2: Iteratively populate GEOGRAM_SRC / ISOTROPICREMESHER_SRC / TBB_SRC**

Run `make -C Libraries/autoremesher_core 2>&1 | head -60`. Read the first missing-symbol or missing-header error. Add the corresponding source file (or `-I` path) to the Makefile. Repeat until the library builds.

The **honest reality** of this step: it may take 30–60 minutes of iteration on a fresh checkout because geogram's build is CMake-native and its dependency graph is not obvious. Options for the person doing this work:

- **Option A (recommended)**: manually walk the call graph — every `#include "geogram/foo.h"` in `src/*.cpp` implies compiling `thirdparty/geogram/src/lib/geogram/foo.cpp` (and transitively). Add sources one at a time.
- **Option B**: if manual walking becomes intractable, write a Bash shim in the Makefile that runs geogram's own CMake in `thirdparty/geogram/build/`, produces `libgeogram.a`, and links our library against that instead. Documented in the spec's "Open item to nail down during extraction" note.

Prefer Option A. Fall back to Option B only if Option A stalls past ~90 minutes.

- [ ] **Step 3: Verify build produces the static lib**

```bash
make -C "Libraries/autoremesher_core" clean
make -C "Libraries/autoremesher_core"
test -f "Libraries/autoremesher_core/libautoremesher_core.a" && echo OK
```

Expected: `OK`.

- [ ] **Step 4: Verify `build-all.sh` picks it up**

Run: `./build-all.sh 2>&1 | grep autoremesher_core | head`

Expected: at least one line showing `autoremesher_core` being built. If `build-all.sh` doesn't pick up the new library automatically, add its build to `build-all.sh` (probably as a line before MSL's build step; check the existing pattern for `Libraries/raylib/`).

- [ ] **Step 5: Commit**

```bash
git add "Libraries/autoremesher_core/Makefile" build-all.sh
git commit -m "build(autoremesher_core): Makefile + build-all.sh integration"
```

---

## Task 5: Public API header `autoremesher/remesh.h`

**Agent Model:** Sonnet implementation, Opus review — header contents are verbatim from the spec. Opus review confirms no drift from the spec's field types/defaults and that the standalone-compile check passes.

**Files:**
- Create: `Libraries/autoremesher_core/include/autoremesher/remesh.h`

**Interfaces:**
- Consumes: nothing (pure header).
- Produces: the entrypoint API used by Task 6 (`remesh.cpp` implements it) and Task 10 (`mesh_retopo.cpp` in MSL calls it).

- [ ] **Step 1: Write the header verbatim from the spec**

Create `Libraries/autoremesher_core/include/autoremesher/remesh.h`:

```cpp
// autoremesher/remesh.h — public API for the vendored headless retopo pipeline.
//
// Single entrypoint: autoremesher::remesh(). Deterministic given (input,
// options). Never throws. See docs/superpowers/specs/2026-07-07-autoremesher-
// integration-design.md for the design rationale.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace autoremesher {

struct Mesh {
    std::vector<float>    positions;  // xyz, xyz, ...  (size = 3 * vertexCount)
    std::vector<uint32_t> indices;    // 3 per triangle
};

struct Options {
    float    target_ratio    = 1.0f;   // relative to input tri count, clamped (0, 4.0]
    int      iterations      = 3;      // upstream default
    uint32_t seed            = 0;      // deterministic RNG seed
    int      timeout_seconds = 60;     // 0 = no limit
    int      threads         = 1;      // pinned for determinism (>= 1)
};

struct Result {
    bool        ok = false;
    Mesh        mesh;
    std::string err;
    double      elapsed_seconds = 0.0;
};

// Runs the sanitize → parameterize → quad-extract → triangulate pipeline.
// Never throws. Never modifies `in`. Deterministic given (in, opts).
Result remesh(const Mesh& in, const Options& opts);

// Version string embedded at compile time. Change this string whenever an
// extraction sync changes the vendored pipeline sources — MSL's cache key
// depends on it.
extern const char* const AUTOREMESHER_CORE_VERSION;

} // namespace autoremesher
```

- [ ] **Step 2: Verify header compiles standalone**

```bash
g++ -std=c++17 -Iinclude -c -x c++ /dev/null -include "Libraries/autoremesher_core/include/autoremesher/remesh.h" -o /dev/null
```

Expected: no output, exit code 0.

- [ ] **Step 3: Commit**

```bash
git add "Libraries/autoremesher_core/include/autoremesher/remesh.h"
git commit -m "feat(autoremesher_core): public API header"
```

---

## Task 6: Driver `src/remesh.cpp` + standalone smoke test

**Agent Model:** Opus — the plan's `ar_internal::` forward declarations are aspirational scaffolding. Real upstream signatures must be discovered by reading the vendored sources from Task 3, and the driver body adapted to match. Failure debugging requires tracing pipeline-stage semantics.

**Files:**
- Create: `Libraries/autoremesher_core/src/remesh.cpp`
- Create: `Libraries/autoremesher_core/tests/smoke_cube.cpp`
- Create: `Libraries/autoremesher_core/tests/Makefile`

**Interfaces:**
- Consumes: pipeline sources from Task 3, public header from Task 5, static lib from Task 4.
- Produces: implementation of `autoremesher::remesh()` and the `AUTOREMESHER_CORE_VERSION` symbol.

- [ ] **Step 1: Write the smoke test first (TDD)**

Create `Libraries/autoremesher_core/tests/smoke_cube.cpp`:

```cpp
// Standalone smoke test: run remesh() on a unit cube, verify basic invariants.
// Not a full test suite — a signal that the library links and produces valid
// output on trivial input.
#include "autoremesher/remesh.h"

#include <cstdio>
#include <cstdlib>

int main() {
    // Unit cube: 8 verts, 12 tris.
    autoremesher::Mesh in;
    in.positions = {
        0,0,0,  1,0,0,  1,1,0,  0,1,0,   // z=0 face
        0,0,1,  1,0,1,  1,1,1,  0,1,1,   // z=1 face
    };
    in.indices = {
        0,1,2, 0,2,3,   // z=0
        4,6,5, 4,7,6,   // z=1
        0,4,5, 0,5,1,   // y=0
        2,6,7, 2,7,3,   // y=1
        1,5,6, 1,6,2,   // x=1
        0,3,7, 0,7,4,   // x=0
    };

    autoremesher::Options opts;
    opts.target_ratio    = 1.0f;
    opts.iterations      = 3;
    opts.seed            = 42;
    opts.timeout_seconds = 30;
    opts.threads         = 1;

    autoremesher::Result r = autoremesher::remesh(in, opts);

    if (!r.ok) {
        std::fprintf(stderr, "FAIL: remesh returned ok=false, err=\"%s\"\n", r.err.c_str());
        return 1;
    }
    if (r.mesh.positions.size() < 12) {   // at least 4 verts
        std::fprintf(stderr, "FAIL: too few positions (%zu)\n", r.mesh.positions.size());
        return 1;
    }
    if (r.mesh.indices.size() % 3 != 0) {
        std::fprintf(stderr, "FAIL: indices not a multiple of 3 (%zu)\n", r.mesh.indices.size());
        return 1;
    }
    if (r.mesh.indices.empty()) {
        std::fprintf(stderr, "FAIL: no output triangles\n");
        return 1;
    }

    std::printf("OK: %zu verts, %zu tris, elapsed=%.3fs\n",
                r.mesh.positions.size() / 3,
                r.mesh.indices.size() / 3,
                r.elapsed_seconds);
    return 0;
}
```

And `Libraries/autoremesher_core/tests/Makefile`:

```makefile
CXX ?= g++
CXXFLAGS := -std=c++17 -O2 -Wall -I../include
LDFLAGS  := -L.. -lautoremesher_core -lpthread

smoke_cube: smoke_cube.cpp ../libautoremesher_core.a
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

.PHONY: clean
clean:
	rm -f smoke_cube
```

- [ ] **Step 2: Run test to verify it fails to link**

Run:
```bash
make -C "Libraries/autoremesher_core/tests" smoke_cube 2>&1 | tail -5
```

Expected: link error — `undefined reference to autoremesher::remesh` and `AUTOREMESHER_CORE_VERSION`. This confirms the header is discovered but no implementation exists.

- [ ] **Step 3: Write driver `src/remesh.cpp`**

Create `Libraries/autoremesher_core/src/remesh.cpp`. The exact contents depend on the pipeline signatures exposed by the vendored files from Task 3, so this is a "compose the pieces" step. Skeleton:

```cpp
// src/remesh.cpp — headless driver that composes the vendored pipeline.
#include "autoremesher/remesh.h"

#include <chrono>
#include <cstdint>
#include <exception>

// Forward-declare the pipeline stages exposed by the vendored files.
// If the vendored .cpp files expose different names, adjust here (and note
// the mapping in UPSTREAM.md).
namespace ar_internal {
    // Returns true on success. `err` set on failure.
    bool sanitize(std::vector<float>& positions,
                  std::vector<uint32_t>& indices,
                  std::string& err);
    // Runs HDC parameterization. Writes cross-field data into scratch buffers.
    bool parameterize(const std::vector<float>& positions,
                      const std::vector<uint32_t>& indices,
                      int iterations, uint32_t seed, int threads,
                      std::vector<float>& scratch, std::string& err);
    // Extracts quad-dominant mesh from parameterization.
    bool extract_quads(const std::vector<float>& src_positions,
                       const std::vector<uint32_t>& src_indices,
                       const std::vector<float>& scratch,
                       float target_ratio,
                       std::vector<float>& out_positions,
                       std::vector<uint32_t>& out_quad_indices,
                       std::string& err);
    // Triangulates quads. Output is 3-per-triangle indices.
    bool triangulate(const std::vector<uint32_t>& quad_indices,
                     std::vector<uint32_t>& tri_indices,
                     std::string& err);
}

namespace autoremesher {

const char* const AUTOREMESHER_CORE_VERSION = "0.1.0-2026-07-07";

Result remesh(const Mesh& in, const Options& opts) {
    Result r;
    auto t0 = std::chrono::steady_clock::now();

    if (in.positions.empty() || in.indices.empty()) {
        r.err = "empty input";
        return r;
    }
    if (in.indices.size() % 3 != 0) {
        r.err = "indices not divisible by 3";
        return r;
    }

    // Clamp target_ratio.
    float ratio = opts.target_ratio;
    if (ratio <= 0.0f) ratio = 1.0f;
    if (ratio > 4.0f)  ratio = 4.0f;

    try {
        std::vector<float>    positions = in.positions;
        std::vector<uint32_t> indices   = in.indices;

        if (!ar_internal::sanitize(positions, indices, r.err))            return r;
        std::vector<float> scratch;
        if (!ar_internal::parameterize(positions, indices,
                                       opts.iterations, opts.seed, opts.threads,
                                       scratch, r.err))                   return r;

        std::vector<float>    out_pos;
        std::vector<uint32_t> quad_idx;
        if (!ar_internal::extract_quads(positions, indices, scratch, ratio,
                                        out_pos, quad_idx, r.err))        return r;

        std::vector<uint32_t> tri_idx;
        if (!ar_internal::triangulate(quad_idx, tri_idx, r.err))          return r;

        r.mesh.positions = std::move(out_pos);
        r.mesh.indices   = std::move(tri_idx);

        // Degenerate-output sanity check (spec Failure Handling table).
        if (r.mesh.indices.size() < 12) {  // fewer than 4 triangles
            r.err = "degenerate output";
            r.mesh = {};
            return r;
        }

        r.ok = true;
    } catch (const std::exception& e) {
        r.err = std::string("exception: ") + e.what();
        r.mesh = {};
    } catch (...) {
        r.err = "unknown exception";
        r.mesh = {};
    }

    auto t1 = std::chrono::steady_clock::now();
    r.elapsed_seconds = std::chrono::duration<double>(t1 - t0).count();

    // Timeout post-check (0 = disabled). We don't kill the running pipeline
    // mid-flight — that requires threading. For v1 we let the pipeline finish
    // and then mark the result as timed-out if it took too long.
    if (opts.timeout_seconds > 0 && r.elapsed_seconds > opts.timeout_seconds) {
        r.ok = false;
        r.err = "timeout after " + std::to_string(opts.timeout_seconds) + "s";
        r.mesh = {};
    }

    return r;
}

} // namespace autoremesher
```

**Note on `ar_internal` names**: this is a scaffold. When the vendored .cpp files from Task 3 expose different function signatures (they will — this driver structure is aspirational), reshape the forward declarations and body to match. Record the mapping in UPSTREAM.md's "Deviations" section.

- [ ] **Step 4: Add `remesh.cpp` to `PIPELINE_SRC` in the Makefile**

Edit `Libraries/autoremesher_core/Makefile`:

```makefile
PIPELINE_SRC := \
    src/remesh.cpp \
    src/mesh_sanitize.cpp \
    src/param_hdc.cpp \
    src/quad_extract.cpp \
    src/quad_to_tri.cpp
```

(This line should already exist from Task 4; confirm `src/remesh.cpp` is present.)

- [ ] **Step 5: Build library, then build and run smoke test**

```bash
make -C "Libraries/autoremesher_core" clean
make -C "Libraries/autoremesher_core"
make -C "Libraries/autoremesher_core/tests" clean
make -C "Libraries/autoremesher_core/tests" smoke_cube
"./Libraries/autoremesher_core/tests/smoke_cube"
```

Expected: `OK: <N> verts, <M> tris, elapsed=<...>s` with N ≥ 4 and M ≥ 4.

If the smoke test fails, the debug loop is: read the error message, trace back to which pipeline stage `ar_internal::` call it corresponds to, verify the vendored source's actual function signature matches the forward declaration in remesh.cpp.

- [ ] **Step 6: Commit**

```bash
git add "Libraries/autoremesher_core/src/remesh.cpp" \
        "Libraries/autoremesher_core/tests/"
git commit -m "feat(autoremesher_core): driver + cube smoke test"
```

---

## Task 7: MSL `MeshIndexed` type + tests

**Agent Model:** Sonnet implementation, Opus review — struct definitions, weld implementation, and TDD tests are fully specified in the plan. Opus review focuses on weld-tolerance edge cases at the epsilon grid boundary (positions that quantize to different keys despite being within epsilon of each other).

**Files:**
- Create: `MatterSurfaceLib/include/mesh_indexed.hpp`
- Create: `MatterSurfaceLib/src/mesh_indexed.cpp`
- Create: `MatterSurfaceLib/tests/mesh_indexed_tests.cpp`
- Modify: `MatterSurfaceLib/main.cpp` — register the new test suite
- Modify: `MatterSurfaceLib/Makefile` — include `src/mesh_indexed.cpp` in sources

**Interfaces:**
- Consumes: `MatterSurfaceLib/include/bvh.h` (`Tri`, `TriEx`, `float3`).
- Produces: `struct MeshIndexed`, `struct WeldOptions`, `MeshIndexed from_tri(...)`, `void to_tri(...)`. Consumed by Tasks 8, 9, 10, 11.

- [ ] **Step 1: Write the header**

Create `MatterSurfaceLib/include/mesh_indexed.hpp`:

```cpp
#ifndef MSL_MESH_INDEXED_HPP
#define MSL_MESH_INDEXED_HPP

#include "bvh.h"  // Tri, TriEx, float3

#include <cstdint>
#include <vector>

// Shared indexed mesh format for MatterSurfaceLib's mesh-transformation
// pipeline. Both mesh_simplifier and mesh_retopo consume and produce this.
// Non-indexed Tri is used only at the BLAS boundary.
struct MeshIndexed {
    std::vector<float3>   positions;
    std::vector<uint32_t> indices;      // 3 per triangle
    std::vector<TriEx>    triex;        // optional; parallel to triangles
                                         // (size == indices.size()/3 when present)
                                         // empty vector = TriEx not attached
};

// Weld tolerance for from_tri. Default matches mesh_simplifier's existing
// internal weld (1e-4 world units).
struct WeldOptions {
    float epsilon = 1e-4f;
};

// Weld the non-indexed Tri list into an indexed MeshIndexed. If `triex` is
// non-null it must be parallel to `tris` (size == tris.size()); triangles keep
// their per-triangle TriEx in the output.
MeshIndexed from_tri(const std::vector<Tri>& tris,
                     const std::vector<TriEx>* triex,
                     const WeldOptions& opts = {});

// Unweld back to non-indexed Tri, one triangle per 3 indices in emission order.
// `triex_out` is populated parallel to `tris_out` iff `in.triex` was populated.
void to_tri(const MeshIndexed& in,
            std::vector<Tri>& tris_out,
            std::vector<TriEx>& triex_out);

#endif // MSL_MESH_INDEXED_HPP
```

- [ ] **Step 2: Write the failing tests**

Create `MatterSurfaceLib/tests/mesh_indexed_tests.cpp`:

```cpp
// Tests for MeshIndexed's from_tri/to_tri round-trip.
#include "mesh_indexed.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

Tri make_tri(float3 a, float3 b, float3 c) {
    Tri t{};
    t.vertex0 = a; t.vertex1 = b; t.vertex2 = c;
    t.centroid = make_float3((a.x+b.x+c.x)/3.0f, (a.y+b.y+c.y)/3.0f, (a.z+b.z+c.z)/3.0f);
    return t;
}

bool near_eq(float a, float b, float eps = 1e-6f) { return std::fabs(a - b) < eps; }
bool near_eq(float3 a, float3 b, float eps = 1e-6f) {
    return near_eq(a.x, b.x, eps) && near_eq(a.y, b.y, eps) && near_eq(a.z, b.z, eps);
}

void test_empty_input_empty_output() {
    std::vector<Tri>   tris;
    MeshIndexed m = from_tri(tris, nullptr);
    assert(m.positions.empty());
    assert(m.indices.empty());
    assert(m.triex.empty());

    std::vector<Tri> back_tris;
    std::vector<TriEx> back_triex;
    to_tri(m, back_tris, back_triex);
    assert(back_tris.empty());
    assert(back_triex.empty());
}

void test_two_triangles_share_edge_weld_reduces_verts() {
    // Two tris sharing an edge; 6 corner slots, 4 unique positions.
    float3 A = make_float3(0,0,0), B = make_float3(1,0,0),
           C = make_float3(1,1,0), D = make_float3(0,1,0);
    std::vector<Tri> tris = { make_tri(A,B,C), make_tri(A,C,D) };
    MeshIndexed m = from_tri(tris, nullptr);
    assert(m.positions.size() == 4);
    assert(m.indices.size() == 6);
    assert(m.triex.empty());
}

void test_round_trip_preserves_triangles() {
    float3 A = make_float3(0,0,0), B = make_float3(1,0,0),
           C = make_float3(1,1,0), D = make_float3(0,1,0);
    std::vector<Tri> tris = { make_tri(A,B,C), make_tri(A,C,D) };
    MeshIndexed m = from_tri(tris, nullptr);

    std::vector<Tri>   out_tris;
    std::vector<TriEx> out_triex;
    to_tri(m, out_tris, out_triex);

    assert(out_tris.size() == 2);
    assert(out_triex.empty());
    // Order preserved: triangle 0 corners are A,B,C.
    assert(near_eq(out_tris[0].vertex0, A));
    assert(near_eq(out_tris[0].vertex1, B));
    assert(near_eq(out_tris[0].vertex2, C));
    assert(near_eq(out_tris[1].vertex0, A));
    assert(near_eq(out_tris[1].vertex1, C));
    assert(near_eq(out_tris[1].vertex2, D));
}

void test_triex_parallel_preserved() {
    float3 A = make_float3(0,0,0), B = make_float3(1,0,0),
           C = make_float3(1,1,0), D = make_float3(0,1,0);
    std::vector<Tri>   tris  = { make_tri(A,B,C), make_tri(A,C,D) };
    std::vector<TriEx> triex(2);
    triex[0].materialId = 7;
    triex[1].materialId = 11;

    MeshIndexed m = from_tri(tris, &triex);
    assert(m.triex.size() == 2);
    assert(m.triex[0].materialId == 7);
    assert(m.triex[1].materialId == 11);

    std::vector<Tri>   out_tris;
    std::vector<TriEx> out_triex;
    to_tri(m, out_tris, out_triex);
    assert(out_triex.size() == 2);
    assert(out_triex[0].materialId == 7);
    assert(out_triex[1].materialId == 11);
}

void test_weld_tolerance() {
    // Two verts within epsilon collapse; two verts outside stay separate.
    float3 A  = make_float3(0.0f, 0.0f, 0.0f);
    float3 A2 = make_float3(0.5e-4f, 0.0f, 0.0f);   // within default 1e-4
    float3 B  = make_float3(1.0f, 0.0f, 0.0f);
    float3 B2 = make_float3(1.0f + 2e-4f, 0.0f, 0.0f); // outside default 1e-4

    std::vector<Tri> tris = {
        make_tri(A,  B,  make_float3(0,1,0)),
        make_tri(A2, B2, make_float3(0,2,0)),
    };
    MeshIndexed m = from_tri(tris, nullptr);
    // A/A2 collapse to one; B and B2 stay separate; two apexes are distinct.
    // Total unique positions: 4.
    assert(m.positions.size() == 4);
}

} // namespace

int run_mesh_indexed_tests() {
    test_empty_input_empty_output();
    test_two_triangles_share_edge_weld_reduces_verts();
    test_round_trip_preserves_triangles();
    test_triex_parallel_preserved();
    test_weld_tolerance();
    std::printf("mesh_indexed_tests: OK (5/5)\n");
    return 0;
}
```

- [ ] **Step 3: Register the test suite in `main.cpp`**

Look at `MatterSurfaceLib/main.cpp` first:

```bash
grep -n "run_.*_tests\|return " "MatterSurfaceLib/main.cpp"
```

Add a declaration and a call to `run_mesh_indexed_tests()` following the existing pattern for other test suites (e.g., `run_mesh_simplifier_tests`). Example:

```cpp
// Near the top of main.cpp:
extern int run_mesh_indexed_tests();

// In main():
if (run_mesh_indexed_tests() != 0) return 1;
```

- [ ] **Step 4: Add source to Makefile**

Look at `MatterSurfaceLib/Makefile`:

```bash
grep -n "src/.*\.cpp\|SRC" "MatterSurfaceLib/Makefile" | head
```

Add `src/mesh_indexed.cpp` and `tests/mesh_indexed_tests.cpp` to the source lists. Follow the exact pattern for existing simplifier sources.

- [ ] **Step 5: Run tests to confirm they fail to link**

```bash
make -C "MatterSurfaceLib" 2>&1 | tail -10
```

Expected: link error — `undefined reference to from_tri(...)` and `to_tri(...)`.

- [ ] **Step 6: Write minimal implementation**

Create `MatterSurfaceLib/src/mesh_indexed.cpp`:

```cpp
#include "mesh_indexed.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <unordered_map>

namespace {

// Quantize a float coordinate to an integer grid key. `epsilon` sets the grid
// spacing: positions closer than `epsilon` are near-guaranteed to map to the
// same key (modulo grid-boundary edge cases, which are acceptable at 1e-4).
struct KeyGen {
    float epsilon;
    long long qx, qy, qz;
    void quantize(float3 p) {
        auto q = [&](float v) -> long long {
            return (long long)std::llround((double)v / (double)epsilon);
        };
        qx = q(p.x); qy = q(p.y); qz = q(p.z);
    }
};

struct KeyHash {
    size_t operator()(const std::array<long long, 3>& k) const noexcept {
        uint64_t h = 14695981039346656037ull;
        const uint8_t* p = reinterpret_cast<const uint8_t*>(k.data());
        for (size_t i = 0; i < sizeof(k); ++i) {
            h ^= p[i];
            h *= 1099511628211ull;
        }
        return (size_t)h;
    }
};

} // namespace

MeshIndexed from_tri(const std::vector<Tri>& tris,
                     const std::vector<TriEx>* triex,
                     const WeldOptions& opts) {
    MeshIndexed out;
    if (tris.empty()) return out;

    std::unordered_map<std::array<long long, 3>, uint32_t, KeyHash> lookup;
    lookup.reserve(tris.size() * 3);

    KeyGen kg{opts.epsilon, 0, 0, 0};

    auto emit_vertex = [&](float3 p) -> uint32_t {
        kg.quantize(p);
        std::array<long long, 3> k = {kg.qx, kg.qy, kg.qz};
        auto it = lookup.find(k);
        if (it != lookup.end()) return it->second;
        uint32_t idx = (uint32_t)out.positions.size();
        out.positions.push_back(p);
        lookup.emplace(k, idx);
        return idx;
    };

    out.indices.reserve(tris.size() * 3);
    for (const Tri& t : tris) {
        out.indices.push_back(emit_vertex(t.vertex0));
        out.indices.push_back(emit_vertex(t.vertex1));
        out.indices.push_back(emit_vertex(t.vertex2));
    }

    if (triex && triex->size() == tris.size()) {
        out.triex = *triex;
    }
    return out;
}

void to_tri(const MeshIndexed& in,
            std::vector<Tri>& tris_out,
            std::vector<TriEx>& triex_out) {
    tris_out.clear();
    triex_out.clear();
    if (in.indices.empty()) return;

    size_t tri_count = in.indices.size() / 3;
    tris_out.reserve(tri_count);

    auto vertex = [&](uint32_t i) -> float3 { return in.positions[i]; };

    for (size_t i = 0; i < tri_count; ++i) {
        float3 a = vertex(in.indices[i*3 + 0]);
        float3 b = vertex(in.indices[i*3 + 1]);
        float3 c = vertex(in.indices[i*3 + 2]);
        Tri t{};
        t.vertex0 = a; t.vertex1 = b; t.vertex2 = c;
        t.centroid = make_float3((a.x+b.x+c.x)/3.0f,
                                 (a.y+b.y+c.y)/3.0f,
                                 (a.z+b.z+c.z)/3.0f);
        tris_out.push_back(t);
    }

    if (in.triex.size() == tri_count) {
        triex_out = in.triex;
    }
}
```

- [ ] **Step 7: Run tests, verify pass**

```bash
make -C "MatterSurfaceLib" && "./MatterSurfaceLib/matter_surface_lib_tests" 2>&1 | tail -20
```

(Adjust the binary name to match whatever the existing MSL Makefile produces; check with `grep -E '\$\(TARGET\)|\-o ' "MatterSurfaceLib/Makefile"`.)

Expected: `mesh_indexed_tests: OK (5/5)` in the output, then whatever the existing suites print.

- [ ] **Step 8: Commit**

```bash
git add "MatterSurfaceLib/include/mesh_indexed.hpp" \
        "MatterSurfaceLib/src/mesh_indexed.cpp" \
        "MatterSurfaceLib/tests/mesh_indexed_tests.cpp" \
        "MatterSurfaceLib/main.cpp" \
        "MatterSurfaceLib/Makefile"
git commit -m "feat(MSL): MeshIndexed type with from_tri/to_tri weld"
```

---

## Task 8: MSL `mesh_transform.hpp` + `reproject_triex`

**Agent Model:** Opus — the plan explicitly flags its O(N*M) fallback as a placeholder to remove. Faithful port of the existing `lod_bake::reproject_triex` spatial-hash algorithm requires reading the current implementation and preserving its exact semantics (cell-size heuristic, tie-breaking).

**Files:**
- Create: `MatterSurfaceLib/include/mesh_transform.hpp`
- Create: `MatterSurfaceLib/src/mesh_transform.cpp`
- Modify: `MatterEngine3/include/lod_bake.h` — remove `reproject_triex` declaration
- Modify: `MatterEngine3/src/lod_bake.cpp` — remove `reproject_triex` implementation
- Modify: `MatterEngine3/src/lod_bake.cpp` — update callers to use MSL version
- Modify: `MatterSurfaceLib/Makefile` — add `src/mesh_transform.cpp`

**Interfaces:**
- Consumes: `MeshIndexed` from Task 7.
- Produces: `void reproject_triex(const MeshIndexed& source, MeshIndexed& target)`. Consumed by Task 10. Also replaces the existing `lod_bake::reproject_triex` implementation.

- [ ] **Step 1: Write the header**

Create `MatterSurfaceLib/include/mesh_transform.hpp`:

```cpp
#ifndef MSL_MESH_TRANSFORM_HPP
#define MSL_MESH_TRANSFORM_HPP

#include "mesh_indexed.hpp"

// Shared TriEx reprojection helper for mesh transformations that change the
// triangle set (simplify, retopo). For each triangle in `target`, finds the
// nearest source triangle by centroid distance and copies its TriEx.
// Semantics match today's lod_bake::reproject_triex — this is a move of that
// logic into MSL so both mesh_simplifier and mesh_retopo can use it.
//
// `source.triex` must be populated and parallel to source triangles (i.e.
// source.triex.size() == source.indices.size()/3). If not, `target.triex` is
// cleared and the function returns without work.
//
// On return, `target.triex` is parallel to `target` triangles.
void reproject_triex(const MeshIndexed& source, MeshIndexed& target);

#endif // MSL_MESH_TRANSFORM_HPP
```

- [ ] **Step 2: Verify the source's existing implementation to preserve semantics**

Read `MatterEngine3/src/lod_bake.cpp` for the `reproject_triex` definition. Note its exact algorithm — likely a uniform spatial hash over source-centroids. The MSL version must preserve those semantics.

```bash
grep -n "reproject_triex" "MatterEngine3/src/lod_bake.cpp" "MatterEngine3/include/lod_bake.h"
```

- [ ] **Step 3: Write MSL implementation**

Create `MatterSurfaceLib/src/mesh_transform.cpp`. Port the existing `lod_bake::reproject_triex` logic verbatim, adapted to the `MeshIndexed` shape:

```cpp
#include "mesh_transform.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>

namespace {

struct CentroidHash {
    // Uniform spatial hash: quantize centroid by cell size, hash the int triple.
    // Cell size is derived from source AABB / target bucket count.
    // Match lod_bake.cpp's existing scheme.
    // (Fill in exact cell-size formula from the source implementation.)
};

float3 centroid_of(const MeshIndexed& m, size_t tri_i) {
    uint32_t i0 = m.indices[tri_i*3 + 0];
    uint32_t i1 = m.indices[tri_i*3 + 1];
    uint32_t i2 = m.indices[tri_i*3 + 2];
    float3 a = m.positions[i0], b = m.positions[i1], c = m.positions[i2];
    return make_float3((a.x+b.x+c.x)/3.0f, (a.y+b.y+c.y)/3.0f, (a.z+b.z+c.z)/3.0f);
}

} // namespace

void reproject_triex(const MeshIndexed& source, MeshIndexed& target) {
    size_t src_tri_count = source.indices.size() / 3;
    size_t tgt_tri_count = target.indices.size() / 3;

    if (src_tri_count == 0 || source.triex.size() != src_tri_count) {
        target.triex.clear();
        return;
    }

    // Port the exact spatial-hash lookup from lod_bake.cpp. Copy the source
    // centroid computation, cell-size heuristic, and nearest-centroid search
    // as-is. Preserve identical semantics so this is a pure code move, not a
    // behavior change.

    target.triex.resize(tgt_tri_count);
    for (size_t i = 0; i < tgt_tri_count; ++i) {
        float3 c = centroid_of(target, i);
        // Nearest-source-centroid search — port from lod_bake.cpp.
        // (Direct copy of the existing algorithm here.)
        size_t best_src = 0;
        float  best_d2  = 1e30f;
        for (size_t j = 0; j < src_tri_count; ++j) {
            float3 sc = centroid_of(source, j);
            float dx = sc.x - c.x, dy = sc.y - c.y, dz = sc.z - c.z;
            float d2 = dx*dx + dy*dy + dz*dz;
            if (d2 < best_d2) { best_d2 = d2; best_src = j; }
        }
        target.triex[i] = source.triex[best_src];
        // Reset shading normals to output triangle's geometric normal, matching
        // lod_bake's existing behavior.
        uint32_t i0 = target.indices[i*3 + 0];
        uint32_t i1 = target.indices[i*3 + 1];
        uint32_t i2 = target.indices[i*3 + 2];
        float3 a = target.positions[i0], b = target.positions[i1], cc = target.positions[i2];
        float3 e1 = make_float3(b.x-a.x, b.y-a.y, b.z-a.z);
        float3 e2 = make_float3(cc.x-a.x, cc.y-a.y, cc.z-a.z);
        float3 n = make_float3(e1.y*e2.z - e1.z*e2.y,
                               e1.z*e2.x - e1.x*e2.z,
                               e1.x*e2.y - e1.y*e2.x);
        float len = std::sqrt(n.x*n.x + n.y*n.y + n.z*n.z);
        if (len > 0.0f) { n.x /= len; n.y /= len; n.z /= len; }
        target.triex[i].N0 = n;
        target.triex[i].N1 = n;
        target.triex[i].N2 = n;
    }
}
```

**Important**: this scaffold uses an O(N*M) nearest-search. If the existing `lod_bake::reproject_triex` uses a spatial hash (spec says it does), port that spatial-hash implementation exactly rather than replacing with the O(N*M) fallback shown here. The naive fallback is a *placeholder to remove* — swap it with the existing algorithm during Step 3.

- [ ] **Step 4: Add source to `MatterSurfaceLib/Makefile`**

Add `src/mesh_transform.cpp` to the source list, same pattern as `src/mesh_indexed.cpp`.

- [ ] **Step 5: Remove `reproject_triex` from lod_bake.h and lod_bake.cpp**

In `MatterEngine3/include/lod_bake.h`, delete the declaration:

```cpp
// DELETE this block:
std::vector<TriEx> reproject_triex(const std::vector<Tri>& out_tris,
                                   const std::vector<Tri>& src_tris,
                                   const std::vector<TriEx>& src_triex);
```

In `MatterEngine3/src/lod_bake.cpp`, delete the definition of `reproject_triex`.

- [ ] **Step 6: Update callers in lod_bake.cpp**

Every internal call to `reproject_triex(std::vector<Tri>...)` inside `lod_bake.cpp` needs to become the MSL-shape call. Add:

```cpp
#include "../../MatterSurfaceLib/include/mesh_transform.hpp"
```

Then wrap the existing call sites. Example transformation — before:

```cpp
auto triex_out = reproject_triex(out_tris, src_tris, src_triex);
```

After — build source and target `MeshIndexed`, call MSL, unwind:

```cpp
MeshIndexed src_m = from_tri(src_tris, &src_triex);
MeshIndexed tgt_m = from_tri(out_tris, nullptr);
reproject_triex(src_m, tgt_m);
std::vector<Tri>   out_tmp;   // unused; we already have out_tris
std::vector<TriEx> triex_out;
to_tri(tgt_m, out_tmp, triex_out);
```

**Note**: this wrapping is verbose because we haven't yet refactored `lod_bake.cpp` to use `MeshIndexed` at its own boundary (that's Task 11). Task 11 will collapse this back into a single `MeshIndexed`-based path.

- [ ] **Step 7: Build and run all tests**

```bash
make -C "MatterSurfaceLib" && "./MatterSurfaceLib/matter_surface_lib_tests"
make -C "MatterEngine3" && "./MatterEngine3/matter_engine3_tests"
```

(Confirm actual test binary names in each Makefile.)

Expected: existing MSL tests pass. Existing MatterEngine3 tests that exercise `bake_lods` still pass — the reproject algorithm is semantically identical.

- [ ] **Step 8: Commit**

```bash
git add "MatterSurfaceLib/include/mesh_transform.hpp" \
        "MatterSurfaceLib/src/mesh_transform.cpp" \
        "MatterSurfaceLib/Makefile" \
        "MatterEngine3/include/lod_bake.h" \
        "MatterEngine3/src/lod_bake.cpp"
git commit -m "refactor: move reproject_triex from lod_bake to MSL"
```

---

## Task 9: MSL `mesh_simplifier` `MeshIndexed` overload

**Agent Model:** Sonnet implementation, Opus review — shim code is fully specified. Opus review checks the `unsigned short` 65535-vert guard, MemAlloc/MemFree pairing (raylib allocator ownership), and the degenerate-output fallback (`if (simplified.vertexCount == 0)`).

**Files:**
- Modify: `MatterSurfaceLib/include/mesh_simplifier.hpp` — add overload declaration
- Modify: `MatterSurfaceLib/src/mesh_simplifier.cpp` — implement overload as a shim
- Modify: `MatterSurfaceLib/tests/mesh_simplifier_tests.cpp` — add overload sanity test

**Interfaces:**
- Consumes: `MeshIndexed` from Task 7, existing `simplify_mesh(raylib::Mesh)` internal path.
- Produces: `MeshIndexed simplify(const MeshIndexed& in, const SimplifyOptions& opts, const CellBounds* bounds = nullptr)`. Consumed by Task 11.

- [ ] **Step 1: Add overload declaration to header**

Append to `MatterSurfaceLib/include/mesh_simplifier.hpp` (after the existing `simplify_mesh` declaration):

```cpp
#include "mesh_indexed.hpp"

// MeshIndexed overload — same semantics as simplify_mesh(raylib::Mesh) above.
// Internally converts to raylib::Mesh, calls the existing implementation, and
// converts back. When lod_bake and other callers migrate to MeshIndexed
// end-to-end (Task 11+), the intermediate raylib::Mesh round-trip goes away.
MeshIndexed simplify(const MeshIndexed& in,
                     const SimplifyOptions& opts,
                     const CellBounds* bounds = nullptr);
```

- [ ] **Step 2: Write failing overload test**

Append to `MatterSurfaceLib/tests/mesh_simplifier_tests.cpp`:

```cpp
void test_simplify_meshindexed_overload_delegates() {
    // Build a small MeshIndexed input, call the overload, verify output shape.
    MeshIndexed in;
    in.positions = {
        make_float3(0,0,0), make_float3(1,0,0), make_float3(1,1,0),
        make_float3(0,1,0), make_float3(1,0,1), make_float3(0,0,1),
    };
    in.indices = { 0,1,2, 0,2,3, 0,1,4, 0,4,5 };

    SimplifyOptions opts;
    opts.target_ratio  = 0.5f;
    opts.lock_boundary = false;

    MeshIndexed out = simplify(in, opts, nullptr);

    // Overload should produce something (may be exactly input if too small to
    // decimate further; we're checking the shim wires up, not decimation).
    assert(!out.positions.empty());
    assert(!out.indices.empty());
    assert(out.indices.size() % 3 == 0);
}
```

Register the call in `run_mesh_simplifier_tests()`.

- [ ] **Step 3: Run tests to confirm link failure**

```bash
make -C "MatterSurfaceLib" 2>&1 | tail -5
```

Expected: `undefined reference to simplify(...)`.

- [ ] **Step 4: Implement the overload as a shim**

Append to `MatterSurfaceLib/src/mesh_simplifier.cpp`:

```cpp
#include "mesh_indexed.hpp"

MeshIndexed simplify(const MeshIndexed& in,
                     const SimplifyOptions& opts,
                     const CellBounds* bounds) {
    MeshIndexed out;
    if (in.indices.empty()) return out;

    // Adapt MeshIndexed → raylib::Mesh (indexed) → call existing simplify_mesh
    // → convert result back to MeshIndexed. All allocations use raylib's
    // MemAlloc; the returned raylib::Mesh owns its buffers until we MemFree.
    Mesh rl{};
    rl.vertexCount   = (int)in.positions.size();
    rl.triangleCount = (int)(in.indices.size() / 3);
    rl.vertices      = (float*)MemAlloc(sizeof(float) * 3 * rl.vertexCount);
    rl.indices       = (unsigned short*)MemAlloc(sizeof(unsigned short) * in.indices.size());

    for (int i = 0; i < rl.vertexCount; ++i) {
        rl.vertices[i*3 + 0] = in.positions[i].x;
        rl.vertices[i*3 + 1] = in.positions[i].y;
        rl.vertices[i*3 + 2] = in.positions[i].z;
    }
    for (size_t i = 0; i < in.indices.size(); ++i) {
        rl.indices[i] = (unsigned short)in.indices[i];
    }
    // NOTE: unsigned short caps vertex count at 65535. For meshes larger than
    // that, this shim path breaks — a follow-up (Task 11 migration) removes
    // the raylib::Mesh intermediate entirely. For now, callers whose flatten
    // meshes exceed 65535 verts get an unimplemented path; guard here:
    if (rl.vertexCount > 65535) {
        MemFree(rl.vertices);
        MemFree(rl.indices);
        // Degrade to identity: return input unchanged.
        return in;
    }

    Mesh simplified = simplify_mesh(rl, opts, bounds);

    if (simplified.vertexCount == 0) {
        // Simplifier degenerate fallback — return input unchanged.
        MemFree(rl.vertices);
        MemFree(rl.indices);
        return in;
    }

    out.positions.reserve(simplified.vertexCount);
    for (int i = 0; i < simplified.vertexCount; ++i) {
        out.positions.push_back(make_float3(simplified.vertices[i*3 + 0],
                                            simplified.vertices[i*3 + 1],
                                            simplified.vertices[i*3 + 2]));
    }
    if (simplified.indices) {
        out.indices.reserve(simplified.triangleCount * 3);
        for (int i = 0; i < simplified.triangleCount * 3; ++i) {
            out.indices.push_back((uint32_t)simplified.indices[i]);
        }
    }

    MemFree(rl.vertices);
    MemFree(rl.indices);
    if (simplified.vertices) MemFree(simplified.vertices);
    if (simplified.indices)  MemFree(simplified.indices);

    return out;
}
```

- [ ] **Step 5: Run tests, verify pass**

```bash
make -C "MatterSurfaceLib" && "./MatterSurfaceLib/matter_surface_lib_tests" 2>&1 | tail -20
```

Expected: `mesh_simplifier_tests` passes including the new overload test.

- [ ] **Step 6: Commit**

```bash
git add "MatterSurfaceLib/include/mesh_simplifier.hpp" \
        "MatterSurfaceLib/src/mesh_simplifier.cpp" \
        "MatterSurfaceLib/tests/mesh_simplifier_tests.cpp"
git commit -m "feat(MSL): mesh_simplifier MeshIndexed overload (shim)"
```

---

## Task 10: MSL `mesh_retopo` module + tests

**Agent Model:** Sonnet implementation, Opus review — module wiring is fully specified. Opus review checks the `MeshIndexed` ↔ `autoremesher::Mesh` conversion for position and index ordering, the failure-fallback path (empty output on `ok=false`), and that `reproject_triex` is called on the retopo'd result rather than the input.

**Files:**
- Create: `MatterSurfaceLib/include/mesh_retopo.hpp`
- Create: `MatterSurfaceLib/src/mesh_retopo.cpp`
- Create: `MatterSurfaceLib/tests/mesh_retopo_tests.cpp`
- Modify: `MatterSurfaceLib/main.cpp` — register the new test suite
- Modify: `MatterSurfaceLib/Makefile` — add source + link against autoremesher_core

**Interfaces:**
- Consumes: `MeshIndexed` (Task 7), `reproject_triex` (Task 8), `autoremesher::remesh` (Task 5/6).
- Produces: `struct RetopoOptions`, `struct RetopoResult`, `RetopoResult retopo(const MeshIndexed& in, const RetopoOptions& opts)`. Consumed by Task 13.

- [ ] **Step 1: Write the header**

Create `MatterSurfaceLib/include/mesh_retopo.hpp`:

```cpp
#ifndef MSL_MESH_RETOPO_HPP
#define MSL_MESH_RETOPO_HPP

#include "mesh_indexed.hpp"

#include <cstdint>
#include <string>

struct RetopoOptions {
    float    target_ratio    = 1.0f;   // relative to input tri count, (0, 4.0]
    int      iterations      = 3;
    uint32_t seed            = 0;
    int      timeout_seconds = 60;     // 0 = no limit
    int      threads         = 1;      // pinned for determinism
};

struct RetopoResult {
    MeshIndexed mesh;              // retopo'd; TriEx repopulated via reproject_triex
    bool        ok = false;
    std::string err;
    double      elapsed_seconds = 0.0;
};

// Wraps autoremesher_core::remesh. Handles:
//   - MeshIndexed → autoremesher::Mesh format adaptation
//   - materialId/tint reprojection via reproject_triex
//   - AO / shading normals left at unbaked defaults; vertex_ao runs downstream
// On failure, ok=false and err populated; caller falls back to input mesh.
RetopoResult retopo(const MeshIndexed& in, const RetopoOptions& opts);

#endif // MSL_MESH_RETOPO_HPP
```

- [ ] **Step 2: Write the failing tests**

Create `MatterSurfaceLib/tests/mesh_retopo_tests.cpp`:

```cpp
#include "mesh_retopo.hpp"
#include "mesh_indexed.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace {

// Unit cube as a MeshIndexed (matches the smoke test in Task 6).
MeshIndexed make_cube() {
    MeshIndexed m;
    m.positions = {
        make_float3(0,0,0), make_float3(1,0,0), make_float3(1,1,0), make_float3(0,1,0),
        make_float3(0,0,1), make_float3(1,0,1), make_float3(1,1,1), make_float3(0,1,1),
    };
    m.indices = {
        0,1,2, 0,2,3, 4,6,5, 4,7,6,
        0,4,5, 0,5,1, 2,6,7, 2,7,3,
        1,5,6, 1,6,2, 0,3,7, 0,7,4,
    };
    return m;
}

void test_empty_input_empty_output() {
    MeshIndexed empty;
    RetopoResult r = retopo(empty, {});
    assert(!r.ok);
    assert(!r.err.empty());
    assert(r.mesh.positions.empty());
    assert(r.mesh.indices.empty());
}

void test_deterministic_output() {
    MeshIndexed in = make_cube();
    RetopoOptions opts;
    opts.seed = 42;
    opts.threads = 1;

    RetopoResult r1 = retopo(in, opts);
    RetopoResult r2 = retopo(in, opts);
    assert(r1.ok);
    assert(r2.ok);
    assert(r1.mesh.positions.size() == r2.mesh.positions.size());
    assert(r1.mesh.indices.size()   == r2.mesh.indices.size());
    for (size_t i = 0; i < r1.mesh.positions.size(); ++i) {
        assert(r1.mesh.positions[i].x == r2.mesh.positions[i].x);
        assert(r1.mesh.positions[i].y == r2.mesh.positions[i].y);
        assert(r1.mesh.positions[i].z == r2.mesh.positions[i].z);
    }
    for (size_t i = 0; i < r1.mesh.indices.size(); ++i) {
        assert(r1.mesh.indices[i] == r2.mesh.indices[i]);
    }
}

void test_triex_materialid_preserved_via_reproject() {
    MeshIndexed in = make_cube();
    in.triex.resize(in.indices.size() / 3);
    for (size_t i = 0; i < in.triex.size(); ++i) {
        in.triex[i].materialId = (int)(i % 3) + 1;   // 1, 2, 3 cycling
    }

    RetopoOptions opts;
    opts.seed = 0;
    RetopoResult r = retopo(in, opts);
    assert(r.ok);
    assert(r.mesh.triex.size() == r.mesh.indices.size() / 3);
    // Every output triangle should have a materialId in {1,2,3} — verifies
    // nearest-centroid transfer wired up.
    for (const TriEx& t : r.mesh.triex) {
        assert(t.materialId >= 1 && t.materialId <= 3);
    }
}

void test_failure_fallback_populates_err() {
    // Degenerate single-triangle input; autoremesher should refuse.
    MeshIndexed in;
    in.positions = { make_float3(0,0,0), make_float3(1,0,0), make_float3(0,1,0) };
    in.indices   = { 0, 1, 2 };
    RetopoResult r = retopo(in, {});
    assert(!r.ok);
    assert(!r.err.empty());
    // Caller uses input unchanged; the mesh field may be empty.
}

void test_timeout_short_budget() {
    MeshIndexed in = make_cube();
    RetopoOptions opts;
    opts.timeout_seconds = 1;   // very tight; cube retopo should still finish
    RetopoResult r = retopo(in, opts);
    // On a healthy machine a cube retopo finishes well under 1s. If it takes
    // longer, that's a signal something's wrong with the vendored library —
    // fail the test to surface it.
    if (!r.ok) {
        std::printf("test_timeout_short_budget: retopo failed err=\"%s\" elapsed=%.3fs\n",
                    r.err.c_str(), r.elapsed_seconds);
    }
    assert(r.ok);
    assert(r.elapsed_seconds < 1.0);
}

} // namespace

int run_mesh_retopo_tests() {
    test_empty_input_empty_output();
    test_deterministic_output();
    test_triex_materialid_preserved_via_reproject();
    test_failure_fallback_populates_err();
    test_timeout_short_budget();
    std::printf("mesh_retopo_tests: OK (5/5)\n");
    return 0;
}
```

Register `run_mesh_retopo_tests` in `main.cpp` alongside the others.

- [ ] **Step 3: Update MatterSurfaceLib Makefile to link autoremesher_core**

Modify `MatterSurfaceLib/Makefile`:

```makefile
# Add somewhere near existing CFLAGS/CXXFLAGS block:
CXXFLAGS += -I../Libraries/autoremesher_core/include

# Add to link line:
LDFLAGS += -L../Libraries/autoremesher_core -lautoremesher_core

# Add to source list:
#   src/mesh_retopo.cpp
#   tests/mesh_retopo_tests.cpp
```

- [ ] **Step 4: Run tests to confirm link failure**

```bash
make -C "MatterSurfaceLib" 2>&1 | tail -10
```

Expected: `undefined reference to retopo(...)`.

- [ ] **Step 5: Write the implementation**

Create `MatterSurfaceLib/src/mesh_retopo.cpp`:

```cpp
#include "mesh_retopo.hpp"
#include "mesh_transform.hpp"

#include "autoremesher/remesh.h"

#include <cstdint>

namespace {

autoremesher::Mesh to_ar_mesh(const MeshIndexed& in) {
    autoremesher::Mesh out;
    out.positions.reserve(in.positions.size() * 3);
    for (float3 p : in.positions) {
        out.positions.push_back(p.x);
        out.positions.push_back(p.y);
        out.positions.push_back(p.z);
    }
    out.indices = in.indices;
    return out;
}

MeshIndexed from_ar_mesh(const autoremesher::Mesh& in) {
    MeshIndexed out;
    size_t vcount = in.positions.size() / 3;
    out.positions.reserve(vcount);
    for (size_t i = 0; i < vcount; ++i) {
        out.positions.push_back(make_float3(in.positions[i*3 + 0],
                                            in.positions[i*3 + 1],
                                            in.positions[i*3 + 2]));
    }
    out.indices = in.indices;
    return out;
}

} // namespace

RetopoResult retopo(const MeshIndexed& in, const RetopoOptions& opts) {
    RetopoResult r;

    if (in.positions.empty() || in.indices.empty()) {
        r.err = "empty input";
        return r;
    }

    autoremesher::Mesh    ar_in  = to_ar_mesh(in);
    autoremesher::Options ar_opts;
    ar_opts.target_ratio    = opts.target_ratio;
    ar_opts.iterations      = opts.iterations;
    ar_opts.seed            = opts.seed;
    ar_opts.timeout_seconds = opts.timeout_seconds;
    ar_opts.threads         = opts.threads;

    autoremesher::Result ar_result = autoremesher::remesh(ar_in, ar_opts);

    r.elapsed_seconds = ar_result.elapsed_seconds;
    if (!ar_result.ok) {
        r.err = ar_result.err;
        return r;
    }

    r.mesh = from_ar_mesh(ar_result.mesh);

    // Reproject materialId + tint from source to output via nearest-centroid.
    // AO and shading normals stay at unbaked defaults — vertex_ao downstream.
    reproject_triex(in, r.mesh);

    r.ok = true;
    return r;
}
```

- [ ] **Step 6: Run tests, verify pass**

```bash
make -C "MatterSurfaceLib" && "./MatterSurfaceLib/matter_surface_lib_tests" 2>&1 | tail -10
```

Expected: `mesh_retopo_tests: OK (5/5)`.

If retopo fails in unexpected ways (e.g. `err="parameterization diverged"` on the cube), the debug loop is: run `smoke_cube` from Task 6 to isolate whether the failure is in autoremesher_core or MSL's adapter. If smoke_cube passes but the MSL test fails, the adapter is at fault (likely a positions/indices conversion bug).

- [ ] **Step 7: Commit**

```bash
git add "MatterSurfaceLib/include/mesh_retopo.hpp" \
        "MatterSurfaceLib/src/mesh_retopo.cpp" \
        "MatterSurfaceLib/tests/mesh_retopo_tests.cpp" \
        "MatterSurfaceLib/main.cpp" \
        "MatterSurfaceLib/Makefile"
git commit -m "feat(MSL): mesh_retopo module wrapping autoremesher_core"
```

---

## Task 11: MatterEngine3 `lod_bake.cpp` refactored to consume `MeshIndexed`

**Agent Model:** Opus — refactoring `bake_lods` and the ladder loop without changing semantics requires reading the existing implementation end-to-end and preserving byte-identical output on existing tests. AABB computation for `use_aabb_bounds=true` must be lifted from the current implementation exactly.

**Files:**
- Modify: `MatterEngine3/src/lod_bake.cpp`
- Modify: `MatterEngine3/include/lod_bake.h` (if any signatures change)

**Interfaces:**
- Consumes: `MeshIndexed`, `simplify(MeshIndexed)` from Task 9, `reproject_triex(MeshIndexed)` from Task 8.
- Produces: no signature change to public bake_lods; internal refactor.

- [ ] **Step 1: Read the existing implementation carefully**

```bash
sed -n '1,120p' "MatterEngine3/src/lod_bake.cpp"
```

The current path (from spec's architecture section): `Tri` list → `tris_to_mesh` (non-indexed `raylib::Mesh`) → `simplify_mesh` (welds internally, returns indexed) → `mesh_to_tris` (back to non-indexed `Tri`). Task 11 replaces this with a single `MeshIndexed` route.

- [ ] **Step 2: Refactor `decimate_tris` to route through MeshIndexed**

Rewrite:

```cpp
std::vector<Tri> decimate_tris(const std::vector<Tri>& tris, float keep_ratio) {
    if (tris.empty()) return {};

    MeshIndexed in = from_tri(tris, nullptr);
    SimplifyOptions opts;
    opts.target_ratio  = keep_ratio;
    opts.lock_boundary = false;

    MeshIndexed out = simplify(in, opts, nullptr);

    std::vector<Tri>   out_tris;
    std::vector<TriEx> out_triex_unused;
    to_tri(out, out_tris, out_triex_unused);
    return out_tris.empty() ? tris : out_tris;
}
```

- [ ] **Step 3: Refactor `decimate_to_error` to route through MeshIndexed**

```cpp
std::vector<Tri> decimate_to_error(const std::vector<Tri>& tris, float epsilon,
                                   bool use_aabb_bounds) {
    if (tris.empty()) return {};

    MeshIndexed in = from_tri(tris, nullptr);
    SimplifyOptions opts;
    opts.target_ratio  = 0.0f;                  // ratio unused — error-driven
    opts.max_error     = epsilon * epsilon;
    opts.lock_boundary = true;

    CellBounds bounds{};
    CellBounds* bounds_ptr = nullptr;
    if (use_aabb_bounds) {
        // Compute mesh AABB (existing code lifts and reuses).
        // ... (paste the AABB computation from the current implementation).
        bounds_ptr = &bounds;
    }

    MeshIndexed out = simplify(in, opts, bounds_ptr);

    std::vector<Tri>   out_tris;
    std::vector<TriEx> out_triex_unused;
    to_tri(out, out_tris, out_triex_unused);
    return out_tris.empty() ? tris : out_tris;
}
```

- [ ] **Step 4: Remove the now-unused `tris_to_mesh` and `mesh_to_tris` helpers**

Delete these static helpers from `lod_bake.cpp`. They're superseded by MSL's `from_tri`/`to_tri`.

- [ ] **Step 5: Update reproject_triex callers to use MeshIndexed shape**

The verbose wrapping added in Task 8 Step 6 collapses to:

```cpp
// Old (Task 8 wrapping):
MeshIndexed src_m = from_tri(src_tris, &src_triex);
MeshIndexed tgt_m = from_tri(out_tris, nullptr);
reproject_triex(src_m, tgt_m);
std::vector<Tri>   out_tmp;
std::vector<TriEx> triex_out;
to_tri(tgt_m, out_tmp, triex_out);

// New (Task 11 collapse): route MeshIndexed through the whole ladder.
// Wherever a level's tris + triex are needed, keep them as MeshIndexed
// throughout the ladder loop. Unwind to Tri only at BLAS registration.
```

Trace every call site in `lod_bake.cpp` and route through MeshIndexed. Details depend on the current shape of the ladder loop — read it in Step 1 and refactor accordingly.

- [ ] **Step 6: Build and run all existing tests**

```bash
make -C "MatterEngine3" && "./MatterEngine3/matter_engine3_tests" 2>&1 | tail -20
```

Expected: existing bake_lods tests, composition tests, and part_flatten tests all pass. LOD ladders produce byte-identical outputs to before (the routing changed, the algorithm didn't).

If any test regresses, the debug loop is: identify the failing assertion, print `MeshIndexed` state at the pipeline stage where the value diverges, verify from_tri/to_tri and the shim overload preserve semantics.

- [ ] **Step 7: Commit**

```bash
git add "MatterEngine3/src/lod_bake.cpp" "MatterEngine3/include/lod_bake.h"
git commit -m "refactor(lod_bake): route through MeshIndexed, remove tris_to_mesh/mesh_to_tris round-trip"
```

---

## Task 12: DSL `retopo` block + `RetopoSettings` on part definition

**Agent Model:** Opus — existing DSL binding pattern (quickjs-ng property parsing, helper macros) must be discovered by reading `dsl_bindings.cpp`. `part_asset_v2` serialization versioning has back-compat implications: existing parts on disk must load cleanly with `retopo` defaulted.

**Files:**
- Modify: `MatterEngine3/include/part_asset_v2.h` — add `RetopoSettings` struct
- Modify: `MatterEngine3/src/part_asset_v2.cpp` — serialize `RetopoSettings`
- Modify: `MatterEngine3/src/dsl_bindings.cpp` — parse the `retopo` JS object

**Interfaces:**
- Consumes: nothing from MSL yet.
- Produces: `struct RetopoSettings` on `part_asset::PartDef` (or wherever `FlattenTargets` lives). Consumed by Task 13.

- [ ] **Step 1: Locate the `FlattenTargets` DSL binding pattern to mirror**

```bash
grep -n "FlattenTargets\|retopo\|LodTargets" "MatterEngine3/src/dsl_bindings.cpp" | head
grep -n "FlattenTargets" "MatterEngine3/include/part_asset_v2.h" "MatterEngine3/src/part_asset_v2.cpp"
```

Read the surrounding code — the retopo binding follows the same JS-object → C++ struct pattern.

- [ ] **Step 2: Add `RetopoSettings` to `part_asset_v2.h`**

Add near the `FlattenTargets` declaration (or wherever per-part settings live):

```cpp
struct RetopoSettings {
    bool     enabled         = false;
    float    target_ratio    = 1.0f;
    int      iterations      = 3;
    uint32_t seed            = 0;
    int      timeout_seconds = 60;
};
```

Add a `RetopoSettings retopo;` field on whatever struct currently holds `FlattenTargets`. This is likely `PartDef` or similar; grep in Step 1 confirms the exact struct.

- [ ] **Step 3: Add DSL binding**

In `dsl_bindings.cpp`, find the code that parses `flatten:` from the JS object. Add parallel parsing for `retopo:`:

```cpp
// Pseudocode (adjust to match actual JS-value API):
JSValue retopo_v = JS_GetPropertyStr(ctx, part_obj, "retopo");
if (!JS_IsUndefined(retopo_v)) {
    JSValue enabled_v         = JS_GetPropertyStr(ctx, retopo_v, "enabled");
    JSValue target_ratio_v    = JS_GetPropertyStr(ctx, retopo_v, "target_ratio");
    JSValue iterations_v      = JS_GetPropertyStr(ctx, retopo_v, "iterations");
    JSValue seed_v            = JS_GetPropertyStr(ctx, retopo_v, "seed");
    JSValue timeout_v         = JS_GetPropertyStr(ctx, retopo_v, "timeout_seconds");

    if (JS_IsBool(enabled_v))       part.retopo.enabled         = JS_ToBool(ctx, enabled_v);
    if (JS_IsNumber(target_ratio_v)) {
        double d; JS_ToFloat64(ctx, &d, target_ratio_v);
        part.retopo.target_ratio = (float)d;
    }
    // ... same pattern for iterations, seed, timeout ...

    // Free the JSValues.
    JS_FreeValue(ctx, enabled_v);
    JS_FreeValue(ctx, target_ratio_v);
    JS_FreeValue(ctx, iterations_v);
    JS_FreeValue(ctx, seed_v);
    JS_FreeValue(ctx, timeout_v);
}
JS_FreeValue(ctx, retopo_v);
```

Adapt to whatever helper macros the existing FlattenTargets binding uses (there's usually a `READ_FLOAT`, `READ_BOOL`, etc.). Match the existing pattern precisely.

- [ ] **Step 4: Add serialization/deserialization to `part_asset_v2.cpp`**

Wherever `FlattenTargets` is serialized/deserialized (search: `grep -n "flatten" "MatterEngine3/src/part_asset_v2.cpp"`), add matching lines for `RetopoSettings`. Both the read and write paths.

If part_asset_v2 uses a versioned schema, bump the version and add read-side back-compat: if an old part file lacks the retopo block, default to `RetopoSettings{}` (which has `enabled=false`).

- [ ] **Step 5: Add a test for parsing**

Add a test to `MatterEngine3/tests/composition_tests.cpp` (or wherever DSL parsing is exercised):

```cpp
void test_retopo_dsl_binding() {
    const char* js = R"(
        part("TestPart", {
            retopo: { enabled: true, target_ratio: 0.5, iterations: 5, seed: 99 }
        });
    )";
    // ... invoke DSL, look up TestPart, assert:
    //   part.retopo.enabled == true
    //   part.retopo.target_ratio == 0.5f
    //   part.retopo.iterations == 5
    //   part.retopo.seed == 99
    //   part.retopo.timeout_seconds == 60  (default, not overridden)
}
```

Register in the test main.

- [ ] **Step 6: Run tests**

```bash
make -C "MatterEngine3" && "./MatterEngine3/matter_engine3_tests"
```

Expected: `test_retopo_dsl_binding` passes.

- [ ] **Step 7: Verify existing schemas still parse**

```bash
"./MatterEngine3/matter_engine3_tests" 2>&1 | grep -E "composition|dsl|schema" | head
```

Expected: no schema-parsing regression. Existing schemas have no `retopo:` block, so they should default cleanly to `enabled=false`.

- [ ] **Step 8: Commit**

```bash
git add "MatterEngine3/include/part_asset_v2.h" \
        "MatterEngine3/src/part_asset_v2.cpp" \
        "MatterEngine3/src/dsl_bindings.cpp" \
        "MatterEngine3/tests/composition_tests.cpp"
git commit -m "feat(dsl): retopo block on part definition + serialization"
```

---

## Task 13: Retopo pipeline hook in `part_flatten` + `.retopo.part` cache

**Agent Model:** Opus — must discover the existing `.flat.part` cache infrastructure (cache dir, hashing scheme, serialization codec) by reading `part_flatten.cpp` and mirror it faithfully. Log-line format must match the spec exactly for greppability; hash-key composition affects cache correctness.

**Files:**
- Modify: `MatterEngine3/src/part_flatten.cpp` — insert retopo call between flatten and QEM ladder
- Modify: `MatterEngine3/Makefile` — link autoremesher_core transitively (should already come via MSL)

**Interfaces:**
- Consumes: `MSL::retopo` (Task 10), `RetopoSettings` on part (Task 12).
- Produces: `.retopo.part` cache artifact, sibling to `.flat.part`. Consumed by Task 14 (integration test) and eventually the runtime path.

- [ ] **Step 1: Locate the flatten pipeline's insertion point**

```bash
grep -n "flat.part\|flatten\|bake_lods" "MatterEngine3/src/part_flatten.cpp" | head -30
```

Find the exact spot where the flatten mesh is finalized and before `bake_lods` (or its equivalent) is invoked.

- [ ] **Step 2: Read the existing `.flat.part` cache write path**

```bash
grep -n "write\|read\|\\.flat\\.part\|cache" "MatterEngine3/src/part_flatten.cpp" | head -20
```

Understand:
- Where the cache directory is (probably a global / config field)
- What hashing function is used for `.flat.part`
- What serialization format `.flat.part` uses

Task 13 mirrors this for `.retopo.part`.

- [ ] **Step 3: Add the retopo hook**

Insert after the flatten mesh is produced but before the LOD ladder generation:

```cpp
#include "../../MatterSurfaceLib/include/mesh_retopo.hpp"
#include "../../MatterSurfaceLib/include/mesh_indexed.hpp"
#include "autoremesher/remesh.h"  // AUTOREMESHER_CORE_VERSION

// After the .flat.part mesh (flat_tris, flat_triex) is computed:
if (part.retopo.enabled) {
    // Compute cache key.
    uint64_t retopo_cache_key = hash_combine(
        flat_part_hash,
        std::hash<std::string>{}(std::string(autoremesher::AUTOREMESHER_CORE_VERSION)),
        hash_combine(
            (uint64_t)part.retopo.target_ratio_bits(),  // reinterpret float bits
            (uint64_t)part.retopo.iterations,
            (uint64_t)part.retopo.seed,
            (uint64_t)part.retopo.timeout_seconds),
        platform_triple_hash()  // e.g., "linux-x86_64" or "windows-cross-x86_64"
    );

    std::string retopo_path = cache_dir + "/" + hex(retopo_cache_key) + ".retopo.part";

    if (file_exists(retopo_path)) {
        // Cache hit: load .retopo.part in place of flat_tris/flat_triex.
        load_retopo_part(retopo_path, flat_tris, flat_triex);
    } else {
        // Cache miss: run retopo.
        MeshIndexed in = from_tri(flat_tris, &flat_triex);
        RetopoOptions ropts;
        ropts.target_ratio    = part.retopo.target_ratio;
        ropts.iterations      = part.retopo.iterations;
        ropts.seed            = part.retopo.seed;
        ropts.timeout_seconds = part.retopo.timeout_seconds;
        ropts.threads         = 1;

        RetopoResult res = retopo(in, ropts);
        if (!res.ok) {
            std::fprintf(stderr,
                "[warn] retopo: part=\"%s\" err=\"%s\" elapsed=%.2fs "
                "→ falling back to unretopo'd mesh\n",
                part.name.c_str(), res.err.c_str(), res.elapsed_seconds);
            // Keep flat_tris / flat_triex unchanged.
        } else {
            // Write cache.
            write_retopo_part(retopo_path, res.mesh);
            // Replace flat_tris / flat_triex with retopo output.
            to_tri(res.mesh, flat_tris, flat_triex);
        }
    }
}

// bake_lods proceeds against (possibly retopo'd) flat_tris, flat_triex.
```

`load_retopo_part` and `write_retopo_part` should reuse the existing `.flat.part` serialization codec — same format, different filename. `hash_combine` and `platform_triple_hash` are whatever helpers `part_flatten.cpp` already uses (or add minimal versions).

- [ ] **Step 4: Add cache-key helpers if missing**

If `part_flatten.cpp` doesn't already have `platform_triple_hash()`, add:

```cpp
namespace {
uint64_t platform_triple_hash() {
#if defined(_WIN32)
    return std::hash<std::string>{}("windows-cross-x86_64");
#elif defined(__linux__)
    return std::hash<std::string>{}("linux-x86_64");
#else
    return std::hash<std::string>{}("unknown-platform");
#endif
}
}
```

- [ ] **Step 5: Add a helper `target_ratio_bits()` on `RetopoSettings`**

In `part_asset_v2.h`:

```cpp
struct RetopoSettings {
    // ... existing fields ...
    uint32_t target_ratio_bits() const {
        uint32_t b;
        std::memcpy(&b, &target_ratio, sizeof(b));
        return b;
    }
};
```

Needed because hash-combining a float directly is fraught; hashing the bit pattern is safe.

- [ ] **Step 6: Build + run existing tests**

```bash
make -C "MatterEngine3" && "./MatterEngine3/matter_engine3_tests"
```

Expected: no regressions on existing tests (all schemas default `enabled=false`, so the new code path is inert).

- [ ] **Step 7: Commit**

```bash
git add "MatterEngine3/src/part_flatten.cpp" \
        "MatterEngine3/include/part_asset_v2.h"
git commit -m "feat(part_flatten): retopo hook + .retopo.part cache read/write"
```

---

## Task 14: Retopo integration test

**Agent Model:** Opus — the integration test relies on hooks (`retopo_invocation_count`, `bake_world`, `make_temp_cache_dir`) that may not exist in the current test harness and require judgment to add. Windows cross-build integration step may require reading `build-all.sh` and mimicking `raylib`/`imgui` patterns.

**Files:**
- Create: `MatterEngine3/tests/retopo_integration_tests.cpp`
- Modify: `MatterEngine3/Makefile` — add source
- Modify: `MatterEngine3/tests/main.cpp` (or equivalent) — register

**Interfaces:**
- Consumes: everything from Tasks 1–13.
- Produces: signal that the end-to-end bake pipeline works with retopo enabled.

- [ ] **Step 1: Write the integration test**

Create `MatterEngine3/tests/retopo_integration_tests.cpp`:

```cpp
// End-to-end bake test: a 2-part world with retopo enabled on Tree only.
// Verifies:
//   - .retopo.part appears in cache for Tree
//   - .retopo.part does NOT appear for terrain (retopo disabled)
//   - QEM ladder builds successfully from the retopo'd Tree LOD0
//   - Second bake is a cache hit (retopo not re-invoked)
//   - Third bake after changing retopo.target_ratio invalidates the cache
#include <cassert>
#include <cstdio>
#include <string>

// Include whatever test scaffolding MatterEngine3 already provides — e.g.,
// a fixture that spins up a temp cache dir, evaluates a DSL script, and
// returns the resulting world.

int test_retopo_end_to_end_bake() {
    // Setup: temp cache dir.
    std::string cache_dir = make_temp_cache_dir();

    // Bake a minimal world: Tree.js with retopo enabled + terrain.js without.
    const char* dsl_script = R"(
        part("Tree", {
            geometry: /* minimal test geometry — cube or small mesh */,
            retopo: { enabled: true, target_ratio: 1.0, seed: 42 }
        });
        part("Terrain", {
            geometry: /* minimal flat plane */,
            // no retopo block; defaults enabled=false
        });
        world([
            place("Tree", [0, 0, 0]),
            place("Terrain", [0, -1, 0]),
        ]);
    )";

    bake_world(dsl_script, cache_dir);

    // Assertion 1: Tree's .retopo.part exists.
    assert(any_file_matches(cache_dir, "*.retopo.part") > 0);

    // Assertion 2: Terrain's .retopo.part does NOT exist.
    // (We need a way to correlate a cache file to a part; either look at the
    // hash filename or check the cache index. Fill in based on existing infra.)

    // Assertion 3: LOD ladder built successfully — Tree has 3 LODs.
    // (Query the part_asset for the Tree's LodLevels.)

    // Assertion 4: second bake is a cache hit — retopo NOT re-invoked.
    // Capture a "retopo invocations" counter (add if not present as a test hook).
    reset_retopo_counter();
    bake_world(dsl_script, cache_dir);
    assert(retopo_invocation_count() == 0);

    // Assertion 5: change retopo.target_ratio in the DSL, bake again → retopo
    // is re-invoked (cache invalidated).
    const char* dsl_script_v2 = R"(
        part("Tree", {
            geometry: /* same */,
            retopo: { enabled: true, target_ratio: 0.75, seed: 42 }
        });
        // ... terrain and world same ...
    )";
    reset_retopo_counter();
    bake_world(dsl_script_v2, cache_dir);
    assert(retopo_invocation_count() == 1);

    // Cleanup.
    remove_dir_recursive(cache_dir);

    return 0;
}

int run_retopo_integration_tests() {
    if (test_retopo_end_to_end_bake() != 0) return 1;
    std::printf("retopo_integration_tests: OK (1/1)\n");
    return 0;
}
```

**Note on test infra**: many of these helpers (`make_temp_cache_dir`, `bake_world`, `reset_retopo_counter`, `retopo_invocation_count`) are hypothetical. If MatterEngine3's test harness doesn't already provide them:

- If cache path helpers don't exist, use `mkdtemp` and delete manually.
- If a bake-driver test entry point doesn't exist, expose one — the DSL evaluator and part_flatten APIs are already public; the harness just needs a thin function that runs them end-to-end.
- If a retopo-invocation counter doesn't exist, add one to `part_flatten.cpp` — a static counter incremented every time `MSL::retopo` is actually called (not on cache hit). Expose via a test-only header.

Iterate on this test scaffolding as needed. The GOAL of the integration test is what matters; the exact helpers are implementation.

- [ ] **Step 2: Register the test**

Modify `MatterEngine3/tests/main.cpp` (or wherever integration test entrypoints are registered) to call `run_retopo_integration_tests()`.

- [ ] **Step 3: Add to Makefile**

Add `tests/retopo_integration_tests.cpp` to the source list in `MatterEngine3/Makefile`.

- [ ] **Step 4: Build and run**

```bash
make -C "MatterEngine3" && "./MatterEngine3/matter_engine3_tests" 2>&1 | tail -30
```

Expected: `retopo_integration_tests: OK (1/1)` alongside existing tests.

- [ ] **Step 5: Windows cross-build check**

```bash
make -C "MatterEngine3" windows
```

Expected: no compile errors. The windows binary picks up the same static lib built with mingw.

If the windows build fails to link autoremesher_core (mingw won't have automatically built it), extend `build-all.sh` to build the library for windows too. Check for existing patterns — `Libraries/raylib/` and `Libraries/imgui/` probably have a windows target that autoremesher_core can mirror.

- [ ] **Step 6: Commit**

```bash
git add "MatterEngine3/tests/retopo_integration_tests.cpp" \
        "MatterEngine3/tests/main.cpp" \
        "MatterEngine3/Makefile" \
        build-all.sh
git commit -m "test(retopo): end-to-end bake integration test"
```

---

## Task 15: E2E — Tree.js retopo opt-in + viewer smoke

**Agent Model:** Opus — visual regression judgment (silhouette cleanliness at LOD1/LOD2, shading smoothness on trunk/branches) requires taste. Tree.js opt-in requires reading the existing schema to place the `retopo` block correctly. Diagnosing meadow-scale bake failures crosses multiple pipeline stages.

**Files:**
- Modify: `MatterEngine3/examples/world_demo/schemas/Tree.js` — add `retopo` block

**Interfaces:**
- Consumes: everything.
- Produces: proof that the meadow world bakes and renders with retopo'd trees.

- [ ] **Step 1: Add opt-in to Tree.js**

Modify `MatterEngine3/examples/world_demo/schemas/Tree.js`. Locate the top-level part definition (`part("Tree", { ... })`). Add a `retopo` block:

```javascript
part("Tree", {
    // ... existing definition ...
    retopo: {
        enabled: true,
        target_ratio: 1.0,
        iterations: 3,
        seed: 42,
        timeout_seconds: 120,   // trees are complex; allow more time
    }
});
```

- [ ] **Step 2: Bake meadow with retopo enabled**

Run (adjust command to match how bakes are normally kicked off in this codebase):

```bash
cd MatterEngine3 && ./viewer --bake meadow 2>&1 | tail -30
```

Watch for:
- `retopo` warning lines (if retopo fails on any Tree instance)
- Cache-write messages for `.retopo.part`
- Total bake time (retopo is slow; expect several minutes)

- [ ] **Step 3: Run viewer via viewer_shots.sh (per memory notes)**

```bash
GALLIUM_DRIVER=d3d12 tools/viewer_shots.sh meadow
```

(GALLIUM_DRIVER per the WSLg GL 4.6 memory. Adjust script name to whatever the current viewer_shots invocation is.)

Expected: the viewer boots, renders meadow with retopo'd Trees, exits cleanly (per the never-leave-viewer-windows-open memory).

- [ ] **Step 4: Visually compare before/after**

Compare the freshly baked screenshots against a reference set (previously captured non-retopo'd meadow). What to look for:
- Cleaner silhouettes on Tree LOD1/LOD2 from mid-far distances.
- Fewer "sparse triangle" artifacts on coarse levels.
- Overall shading quality — smoother normals should reduce noise on the trunk / branches.

Failure modes worth noting:
- Silhouette WORSE at LOD0 — likely a TriEx transfer bug (materialId not reprojected correctly).
- Trees invisible or badly stretched — likely a cache miss ⇒ retopo failure ⇒ fallback to unretopo'd, look for the warning log line.

- [ ] **Step 5: Verify Windows build still runs**

```bash
make -C "MatterEngine3" windows
```

Then optionally run the Windows viewer on a Windows host (matches the "always rebuild the Windows binary" memory).

- [ ] **Step 6: Commit**

```bash
git add "MatterEngine3/examples/world_demo/schemas/Tree.js"
git commit -m "feat(meadow): opt Tree into retopo (first schema with retopo enabled)"
```

- [ ] **Step 7: Update ROADMAP.md**

Add a `[DONE]` entry near the top of `ROADMAP.md`:

```markdown
## autoremesher integration [DONE]

Vendored MIT-licensed headless subset of huxingyi/autoremesher into
`Libraries/autoremesher_core/`. Added `MeshIndexed`/`MeshTransform` boundary
in MatterSurfaceLib and `mesh_retopo` module alongside the QEM simplifier.
Retopo is per-part opt-in via `retopo: { enabled: true, ... }` on the DSL
part definition. First opt-in: Meadow's Tree. Design:
`docs/superpowers/specs/2026-07-07-autoremesher-integration-design.md`.
Plan: `docs/superpowers/plans/2026-07-07-autoremesher-integration.md`.

Follow-ups (out of scope for this landing): mesher-native indexed emit
(remove Tri-boundary conversion at MSL entrance), per-cluster retopo with
boundary preservation, subprocess isolation for crash safety.
```

```bash
git add ROADMAP.md
git commit -m "docs: ROADMAP entry for autoremesher integration"
```

---

## Self-Review Notes

Ran the mental checks against the spec:

1. **Spec coverage** — every spec section maps to at least one task:
   - Vendored library: Tasks 1–6
   - MSL additions: Tasks 7–10
   - MatterEngine3 wiring: Tasks 11–13
   - Schema opt-in + cache: Tasks 12, 13
   - Failure handling: Task 10 tests + Task 13 warn-log path
   - Determinism: Task 10 tests + `threads=1` default across
   - Testing: Tasks 7, 10, 12, 14 (unit + integration) + Task 15 (E2E)
   - Migration / rollout: Task 15 (Tree.js opt-in)

2. **Placeholder scan** — the "iteratively populate GEOGRAM_SRC" step in Task 4 is
   an honest investigation step, not a placeholder (it names the exact debug
   loop and fallback). The `ar_internal::` forward-declaration scaffolding in
   Task 6 Step 3 is aspirational and calls this out explicitly. The
   `reproject_triex` naive-O(N*M) in Task 8 Step 3 is flagged as a placeholder
   to remove during the implementation, with instructions to port the existing
   spatial-hash algorithm exactly.

3. **Type consistency** — spot-checked:
   - `MeshIndexed` fields (positions/indices/triex) consistent across Tasks 7,
     8, 9, 10, 11.
   - `RetopoOptions` fields identical between Task 5 (autoremesher::Options),
     Task 10 (RetopoOptions), and Task 12 (RetopoSettings). RetopoSettings adds
     `enabled` but is otherwise a subset.
   - `reproject_triex(source, target)` signature identical across Tasks 8, 10, 11.
   - `AUTOREMESHER_CORE_VERSION` symbol declared in Task 5 header, defined in
     Task 6 driver, referenced in Task 13 cache key.

4. **Scope** — this is one cohesive plan. Tasks have real dependencies but the
   arc is single-feature. No sub-plans warranted.
