# Live terrain in the editor: qualification (clear-crest-48.9.10)

Deliverable `live-terrain-editor-qualification`. This child connects the
.9.9 `LiveTerrain` driver to the real editor and records the native draw.
Before this child, `git grep LiveTerrain` found only its definitions and
`live_terrain_tests.cpp`. Now the editor drives it once per frame. The tiles
it produces reach the coordinator cache, the installed Box3D terrain
collision and the .9.8 `PortableScene` graphics service. The native Linux
editor rasterizes those same tiles through `VulkanRaster`.

Source: base `e57d9bc8` (= `origin/aq/clear-crest-48.9`, which contains
.9.4 `425bce35`, .9.8 `cfe95490` and .9.9 `9b315012`), plus the single
commit on `aq/clear-crest-48.9.10` that carries this document.

## 1. Call chain (production entrypoint)

The fixture is the authored world `LiveTerrainFixture`
(`projects/world_demo/scenes/examples/LiveTerrainFixture/LiveTerrainFixture.js`).
It uses 16 m sectors, and its `field()` stays inside the portable CPU
generator's op set: `noise2`, `mul`, `add` and `heightToDensity`. Live
terrain is opt-in with `MATTER_LIVE_TERRAIN=1`, in the same way
`MATTER_PORTABLE_SCENE_CAPTURE` opts into the portable scene.

**World install (worker thread).** `WorldSession::Impl::install_world`
(`MatterEngine3/src/matter_engine.cpp`, after the river overlay is installed)
publishes `r.field_program` under `live_terrain_source_mutex` and bumps
`live_terrain_revision`. A world with a river height overlay publishes an
empty program instead, because the portable generator samples the bare
program and the tiles would disagree with the installed field. That world
logs `live terrain: river overlays are unsupported`.

**Every frame (app thread).** The editor reaches the live terrain update
through this chain:

1. `MatterEditor/src/main.cpp`: `main()` copies
   `options.live_terrain_graphics = stats.live_terrain_graphics`. This is the
   editor property `viewer.debug.live_terrain_graphics`, declared in
   `MatterEditor/src/editor_props.cpp` and settable over the command FIFO.
2. `WorldSession::render` (`matter_engine.cpp`, the `build.portable_scene`
   block) runs `PROFILE_SCOPE("build.live_terrain")` and then calls
   `impl_->update_live_terrain(cam.position, opts.live_terrain_graphics)`.
   This happens just before `portable_scene->update(...)`.
3. `WorldSession::Impl::update_live_terrain` (`matter_engine.cpp`):
   1. **Construct.** On the first frame it builds
      `terrain_coordinator::LiveTerrainSession` on
      `portable_scene->service()`, the same `GraphicsService` (MatterGraphics
      `SceneBackend` on the leased `VulkanResourceDevice`) that the
      portable parts use.
   2. **Apply the program.** It applies a new program with
      `LiveTerrainSession::set_source(program, "world.<rev>")`. That builds
      the .9.9 request policy and calls `LiveTerrain::set_source`. An edit
      redefines every tile; an unchanged program is a no-op.
   3. **Pump.** `LiveTerrainSession::update(eye, graphics)` calls
      `LiveTerrain::set_anchor`. When the property flips, it also calls
      `drop_graphics()` or `demand_graphics()`. It then runs
      `LiveTerrain::pump(service)`: coordinator dispatch, then
      `CpuTerrain::generate`, then `complete`, then the cache, then
      `pump_graphics` uploads.
   4. **Place.** `LiveTerrain::scene_delta(upsert, remove)` feeds
      `PortableScene::place(upsert, remove)`. Each instance id is the
      coordinator region id (`terrain.live.<x>.0.<z>`), and its handle is
      the GPU handle of that region's coordinator product.
   5. **Install collision.** `LiveTerrainSession::collision_changed(installed_key, ...)`
      calls `LiveTerrain::collision`, which builds a
      `TerrainCollisionCandidate` from the same products' collision
      resources. When the key differs from the physics context's
      `terrain_collision_stats().installation_key`, the frame calls
      `PhysicsContext::replace_terrain_collision`. This runs under
      `terrain_collision_generation_mutex`, the lock the legacy publisher
      also holds. A legacy publication that replaces live collision is
      therefore re-replaced on the next frame.
4. `PortableScene::update` merges the placed instances into its next
   `SceneUpdate`. That function is in
   `MatterEngine3/src/component_adapters/graphics/portable_scene.cpp`, with
   `place()` keeping a map named `external`. With
   `MATTER_PORTABLE_SCENE_CAPTURE=<prefix>`, `update` rasterizes the
   `SceneFrame` with `VulkanRaster` once the scene has been stable, and
   writes `<prefix>.json`, `.albedo.png`, `.depth.pfm`, `.normal.pfm` and
   `.identity.pfm`.

**Teardown.** `WorldSession::~WorldSession` resets `live_terrain` before
`portable_scene`, because the session uploads through the scene's service.
A later install whose program is refused is handled in
`update_live_terrain`: it drops graphics, unplaces the tiles, and resets the
session.

## 2. Native editor draw (Linux, VulkanRaster)

### Environment

- **Device:** `llvmpipe (LLVM 20.1.2, 256 bits)`, Mesa 25.2.8-0ubuntu0.24.04.2,
  Vulkan API 1.4.318. This is the lavapipe software rasterizer, as reported
  by the editor's `Vulkan adapter:` line and by `vulkaninfo --summary`.
- **Host:** WSL2 kernel 6.18.33.2-microsoft-standard-WSL2, Ubuntu 24.04,
  1 CPU (`nproc`).
- **Toolchain:** g++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0, through the
  repository's Make path (`make -C MatterEditor linux`).
- **Validation layers:** none. `VK_LAYER_KHRONOS_validation` is not
  installed; `/usr/share/vulkan/explicit_layer.d` holds only
  `VkLayer_INTEL_nullhw.json` and `VkLayer_MESA_overlay.json`. The run is
  therefore not validation-clean evidence.
- **Root CMake:** unavailable on Linux. `CMakeLists.txt:6` and
  `cmake/MatterCompiler.cmake:8` refuse non-MSVC hosts, so the viewer CMake
  graph edits (§5) were checked by inspection plus the manifest checker, not
  by configuring.
- **glslc:** none on the host. I used `glslc` from Ubuntu's `glslc` /
  `libshaderc1` 2023.8 packages, unpacked with `dpkg -x` into a scratch
  directory (shaderc 2023.8-1build1, glslang 14.0.0-2). `libvulkan.so` was a
  scratch symlink to the system `libvulkan.so.1`. Box3D was
  `third_party/box3d` built into a scratch `libbox3d.a`.

### Build

```bash
make -C MatterEditor linux RETOPO=0 \
  VULKAN_INCLUDE=$PWD/third_party/Vulkan-Headers/include \
  VULKAN_LIB_DIR=<scratch>/vklib BOX3D_LIB=<scratch>/box3d/libbox3d.a \
  GLSLC=<scratch>/bin/glslc
# exit 0 -> MatterEditor/build/linux/editor
```

### Run

The run starts with graphics on, turns graphics off, then turns them back on,
all in one editor process. This is `run_fixture.sh`, reproduced here:

```bash
cd MatterEditor
DISPLAY=:0 MATTER_DISABLE_VK_RT=1 MATTER_VK_TEST_FORCE_RT_UNAVAILABLE=1 MATTER_VOLUMETRICS=0 \
MATTER_HIDE_WINDOW=1 MATTER_HIDE_UI=1 MATTER_WINDOW_WIDTH=480 MATTER_WINDOW_HEIGHT=270 \
MATTER_WORLD=LiveTerrainFixture MATTER_CACHE_ROOT=$R/cache MATTER_PORTABLE_SCENE_CAPTURE=$R/portable \
MATTER_CMD_FIFO=$R/cmd.fifo MATTER_LIVE_TERRAIN=1 ./build/linux/editor > $R/editor.log 2>&1 &
# wait for $R/portable.json            -> copy to on/
echo "set viewer.debug.live_terrain_graphics false" > $R/cmd.fifo
# wait for a newer capture             -> copy to off/
echo "set viewer.debug.live_terrain_graphics true"  > $R/cmd.fifo
# wait for a newer capture             -> copy to back/
echo quit > $R/cmd.fifo
```

Each capture was measured with the stdlib-only script
`docs/component-separation/live-terrain-editor-qualification/measure_capture.py`:

```bash
python3 -I docs/component-separation/live-terrain-editor-qualification/measure_capture.py $R/on/portable
```

The script asserts that each draw's pixel count from the identity image
equals the JSON's `pixels`. It then prints the raster ids and the depth
range measured on exactly those pixels.

### Results (fixture `LiveTerrainFixture`, run `fx2`)

The script printed `captured on / captured off / captured back` and
`editor exit=0`. The committed copies are in
`docs/component-separation/live-terrain-editor-qualification/`:
`{on,off,back}/portable.json`, `{on,off,back}/portable.albedo.png`,
`{on,off,back}/measure.txt` and `editor-log-excerpt.txt`. The 0.5–1.5 MB
identity, depth and normal PFMs are not committed. Scratch paths are
rewritten as `<run>/`.

Editor log, in order:

```text
Vulkan adapter: llvmpipe (LLVM 20.1.2, 256 bits) | driver: llvmpipe (Mesa 25.2.8-0ubuntu0.24.04.2 (LLVM 20.1.2), 0x06402008) | API 1.4.318
[terrain] live terrain collision installed: key=3909000712764386650 shapes=25 regions=25
[terrain] live terrain scene: +8 -0 regions=25 generated=25 cached=25 collision=3909000712764386650
[terrain] live terrain scene: +8 -0 regions=25 generated=25 cached=25 collision=3909000712764386650
[terrain] live terrain scene: +8 -0 regions=25 generated=25 cached=25 collision=3909000712764386650
[terrain] live terrain scene: +1 -0 regions=25 generated=25 cached=25 collision=3909000712764386650
[graphics] portable scene captured 11 draws at revision 4 to <run>/portable
# set viewer.debug.live_terrain_graphics false
[terrain] live terrain scene: +0 -25 regions=25 generated=25 cached=25 collision=3909000712764386650
[graphics] portable scene captured 0 draws at revision 5 to <run>/portable
# set viewer.debug.live_terrain_graphics true
[terrain] live terrain scene: +8 -0 regions=25 generated=25 cached=25 collision=3909000712764386650
[terrain] live terrain scene: +8 -0 regions=25 generated=25 cached=25 collision=3909000712764386650
[terrain] live terrain scene: +8 -0 regions=25 generated=25 cached=25 collision=3909000712764386650
[terrain] live terrain scene: +1 -0 regions=25 generated=25 cached=25 collision=3909000712764386650
[graphics] portable scene captured 11 draws at revision 9 to <run>/portable
```

Measurement of the `on` capture (`on/measure.txt`, all checks passed):

```text
revision 4 480x270 draws=11 refused=[] refused_placements=[]
draw  0 terrain.live.-1.0.-1  raster=terrain.live.-1.0.-1  prototype=terrain.mesh  pixels=  6226 raster_ids=[0]  depth=[0.0021172, 0.0030299] resident_bytes=14876
draw  1 terrain.live.-1.0.0   raster=terrain.live.-1.0.0   prototype=terrain.mesh  pixels= 24538 raster_ids=[1]  depth=[0.0030303, 0.0053110] resident_bytes=29774
draw  2 terrain.live.-1.0.1   raster=terrain.live.-1.0.1   prototype=terrain.mesh  pixels=  9600 raster_ids=[2]  depth=[0.0053144, 0.0060882] resident_bytes=29648
draw  3 terrain.live.-2.0.-1  raster=terrain.live.-2.0.-1  prototype=terrain.mesh  pixels=  4737 raster_ids=[3]  depth=[0.0021172, 0.0030294] resident_bytes=12665
draw  4 terrain.live.-2.0.0   raster=terrain.live.-2.0.0   prototype=terrain.mesh  pixels=  3240 raster_ids=[4]  depth=[0.0030303, 0.0045828] resident_bytes=14906
draw  5 terrain.live.0.0.-1   raster=terrain.live.0.0.-1   prototype=terrain.mesh  pixels=  6220 raster_ids=[5]  depth=[0.0021172, 0.0030301] resident_bytes=14827
draw  6 terrain.live.0.0.0    raster=terrain.live.0.0.0    prototype=terrain.mesh  pixels= 24434 raster_ids=[6]  depth=[0.0030303, 0.0053180] resident_bytes=29797
draw  7 terrain.live.0.0.1    raster=terrain.live.0.0.1    prototype=terrain.mesh  pixels=  9708 raster_ids=[7]  depth=[0.0053183, 0.0061135] resident_bytes=29841
draw  8 terrain.live.1.0.-1   raster=terrain.live.1.0.-1   prototype=terrain.mesh  pixels=  4751 raster_ids=[8]  depth=[0.0021173, 0.0030285] resident_bytes=14929
draw  9 terrain.live.1.0.0    raster=terrain.live.1.0.0    prototype=terrain.mesh  pixels=  3205 raster_ids=[9]  depth=[0.0030346, 0.0045704] resident_bytes=29784
draw 10 terrain.live.2.0.-1   raster=terrain.live.2.0.-1   prototype=terrain.mesh  pixels=    79 raster_ids=[10] depth=[0.0021172, 0.0022724] resident_bytes=12691
background pixels=32862 depth=[0.0, 0.0]
terrain draws with pixels: 11
```

(Depths are rounded here; `measure.txt` has full precision.) The `off`
capture is `revision 5 draws=0`, background 129600 px (the whole 480×270
frame) at depth 0. The `back` capture is `revision 9` with the same 11 draws,
pixel counts and depth ranges, and its identity and depth PFMs are
byte-identical to `on` (`cmp` reported no difference).

**What this shows, for the same resource.**

- **Coverage.** All 11 tiles in the view frustum have pixels: 96,738 of
  129,600 px. The other 14 of the 25 tiles are outside the frustum (see §4,
  "Visible tiles only").
- **Identity.** For every draw, the instance id, the `raster_identity` and
  the identity image's raster id all name the same tile. The instance id is
  the coordinator region id `terrain.live.<x>.0.<z>`. The draw's
  `prototype` is `terrain.mesh` and its material is `terrain.material.1`,
  the .9.9 graphics product of that region.
- **Depth.** Reversed-Z depth is non-zero on every terrain pixel and 0 on
  every background pixel. Depth increases toward the camera: the near row
  z=1 is deepest (≈0.0061) and the far row z=-1 is shallowest (≈0.0021).
  Adjacent tiles' depth ranges abut, for example 0.0030299 / 0.0030303 at
  the z=-1 / z=0 boundary.
- **Collision.** It is installed from the same 25 regions' collision
  products: `shapes=25 regions=25`, key 3909000712764386650. That key is
  unchanged through graphics-off and back, so the collision was not
  reinstalled.
- **Eviction and reuse.** Graphics-off removes all 25 placements (`+0 -25`)
  while `cached=25` and the collision key hold. Graphics-on re-places all 25
  with `generated=25` unchanged, so no tile was re-sampled: the products came
  from the coordinator cache.

## 3. Lifecycle proof through the adapter

`test_live_terrain_session` in `MatterEngine3/tests/live_terrain_tests.cpp`
drives `LiveTerrainSession`, the class that `update_live_terrain` calls. It
makes the same calls in the same order: `set_source`, then
`update(anchor, graphics)`, then `scene_delta`, then
`collision_changed` / `replace_terrain_collision`. The graphics side is a
MatterGraphics `Graphics`/`SceneBackend` on the test device, and collision is
a real Box3D `PhysicsContext`. The test proves:

| Claim | Check |
|---|---|
| Rings and placement | 25 regions, all 25 tiles placed, collision installed once and then reported current |
| Adjacent seam | `same_seam` at x=-16 |
| 2:1 detail seam | `same_seam` at z=16 |
| Refined seam after the anchor moves | `same_seam` at x=16 |
| Unchanged program is not an edit | Same program at revision world.2 redefines nothing |
| Graphics eviction | Graphics off gives 0 upserts / 25 removes. Every tile keeps its collision lease and loses its GPU residency, the installation key is unchanged, and the cache size is unchanged |
| Repeated demand reuses resources | Graphics back on re-places 25 tiles with identical product digests, an identical cache size and `generated()` unchanged (no CPU generation) |
| Anchor demand update | Moving one tile east drops x=-3, adds x=2 and refines (1,0,-1) to detail 0. Collision is reinstalled, the scene follows (≥5 removes), and new tiles are generated |
| Returning anchor reuses the cache | Identical digests, cache size and `generated()`. The collision key returns to the original installation |
| Edit rejects outstanding work | An edit dispatches a job, and a second edit cancels it |
| Edit rejects late work | The replaced program's request is refused by the current source (`missing_dependency`). Its late completion is rejected as `stale_result` |
| Latest program wins | Every tile publishes `world.4` and its content. The collision installation changes and all 25 tiles are re-placed |

`generated()` counts the tiles that the CPU generator actually sampled; a
cache hit does not count. This counter is what makes "reuse" checkable
rather than inferred from cache size.

The native run in §2 shows the same eviction and re-demand on the real
editor path. The log lines `live terrain scene: +N -M regions=… generated=…
cached=… collision=…` record them.

### Commands and results

```bash
MATTER_CONTOUR_SEAMS=1 make -C MatterEngine3/tests run-live-terrain BOX3D_LIB=<scratch>/box3d/libbox3d.a
```

```text
MATTER_CONTOUR_SEAMS=1 ./build/live_terrain_tests
== live terrain: resolve, cache, collide, render ==
  adjacent equal-detail seam at x=-16: 9 shared seam edges
  2:1 detail seam at z=16: 9 shared seam edges
  frame: 25 visible of 25 resident tiles
  sphere at (-14.22, 7.89, -8.76), 0.521 m above the surface
  refined equal-detail seam at x=16: 8 shared seam edges
  moved 2:1 seam at z=16: 9 shared seam edges
== live terrain session: the editor's adapter lifecycle ==
  session adjacent seam at x=-16: 9 shared seam edges
  session 2:1 seam at z=16: 9 shared seam edges
  session refined seam at x=16: 8 shared seam edges
== browser: terrain capability unsupported ==
ALL PASS
# make exit 0
```

The first and third groups are the .9.9 tests, unchanged. The session group
is new, and it adds checks without editing or loosening any existing check
or tolerance.

The .9.9 regression suites were each built standalone and run with ctest:

```bash
cmake -S <dir> -B <scratch>/ct_<name> -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build <scratch>/ct_<name> -j1
ctest --test-dir <scratch>/ct_<name>
```

| Suite (`<dir>`) | Result |
|---|---|
| `libs/TerrainGeneration` | 24/24 passed |
| `libs/MatterGraphics` | 3/3 passed (lifetime, scene, composition) |
| `MatterEngine3/src/component_adapters/terrain_coordinator` | 7/7 passed |

These match the .9.9 evidence (Terrain 24/24, Graphics 3/3,
coordinator 7/7).

Repository checkers:

- `python3 tools/component-separation/check_dependencies.py` gives
  85 PASS / 11 FAIL / 39 UNTESTED, exit 1. A detached worktree at the base
  commit e57d9bc8 gives the identical per-check result set. This change adds
  no failure and fixes none.
- `python3 tools/check-source-manifests.py --root .` reports the same 3
  errors as the base: `collision/terrain.cpp`, `animation/native.cpp` and
  `mem_bank.c`. These are entries that already existed, and none comes from
  the sources this change adds to `engine-viewer.sources`.

## 4. Limitations

- **Presentation.** The swapchain still presents the legacy renderer.
  Terrain pixel, identity and depth evidence comes from the existing
  `PortableScene` `VulkanRaster` capture seam (.9.8), not from the window.
  The capture fires once the scene is stable. Any later scene change re-arms
  it and overwrites the same prefix, which is why the run copies each phase
  out.
- **Opt-in only.** Live terrain is opt-in (`MATTER_LIVE_TERRAIN=1`). The
  default editor path is unchanged. With live terrain on, the legacy sector
  terrain is still baked and drawn by the legacy renderer.
- **Programs and overlays.** Programs outside the portable CPU generator's
  op set are refused at `set_source` and logged once. Worlds with river
  height overlays are refused at install.
- **Visible tiles only.** Only GPU-resident tiles that pass the raster's own
  culling are drawn, so a capture shows the visible subset of the 25
  resident tiles.
- **The Crate root.** It is authored to opt into the portable route, but it
  produced no upload in this world-kind session. The capture's `assets` list
  is empty and every draw is a terrain tile. The cause is outside this
  child's scope and was not investigated. This does not affect the terrain
  evidence.
- **Cache sizing.** `MemoryCache` reserves each entry's declared budget
  (output + resource + document = 192 MiB per tile), not its real size. The
  session therefore sizes its cache as 256 entries × 192 MiB, which makes
  the entry count the effective bound. A smaller byte budget silently held
  only two tiles and thrashed.
- **No validation layer and no GPU.** The run used a software rasterizer
  (llvmpipe) with no validation layer and no hardware GPU. Windows/MSVC was
  not built or run here.
- **NOT RUN, per the 2026-10-20 supervisor disposition.** This host has no
  display server, no `/dev/dri`, no hardware ray-tracing runner and no
  prebuilt editor. The following are therefore **NOT RUN**:
  - **Native UI.** No presented window or swapchain image was inspected. The
    editor ran with `MATTER_HIDE_WINDOW=1 MATTER_HIDE_UI=1`, and all pixel
    evidence is from the offscreen `VulkanRaster` capture.
  - **Hardware ray tracing.** It was disabled with `MATTER_DISABLE_VK_RT=1`
    and `MATTER_VK_TEST_FORCE_RT_UNAVAILABLE=1`.
  - **Device performance.** The `[bake-timing]` figures are from a
    one-CPU software rasterizer and are not performance evidence.

  Real-device acceptance remains open for hardware later. What §2 proves is
  the headless software-Vulkan draw.

**Browser terrain remains unsupported.** The browser viewer's
capability table still declares `matter.asset.terrain` as
`support: 'unsupported'`
(`integrations/web-procedural/viewer/src/capabilities.mts`).
`test_browser_reports_terrain_unsupported` asserts this. This child adds
nothing browser-side and claims no browser terrain.

## 5. Changed lines: moved vs new

Moved: **0**. Everything below is new wiring. No terrain meshing, seam,
coordinator, renderer or shader source changed.

| File | +/- | What |
|---|---|---|
| `MatterEngine3/src/component_adapters/terrain_coordinator/engine/live_terrain_session.h` | +51 (new) | The editor's live terrain owner: cache, source service, `LiveTerrain` |
| `.../engine/live_terrain_session.cpp` | +80 (new) | See the justification below |
| `MatterEngine3/src/matter_engine.cpp` | +102 | Members, `update_live_terrain`, the install publish, the render call and teardown order |
| `MatterEngine3/include/matter/world_session.h` | +5 | `RenderOptions::live_terrain_graphics` |
| `MatterEngine3/src/component_adapters/graphics/portable_scene.{h,cpp}` | +33/-3 | `service()`, `place()`, merging external instances, capture re-arm after the first publish |
| `MatterEditor/src/{main.cpp,ui.h,editor_props.cpp}` | +9 | The `viewer.debug.live_terrain_graphics` property |
| `MatterEngine3/Makefile` | +3/-1 | vpath and viewer include paths for the coordinator and TerrainGeneration |
| `cmake/manifests/engine-viewer.sources` | +6 | `live_terrain{,_session}.cpp`, `coordinator.cpp`, `cache.cpp`, TerrainGeneration `field.cpp` and `meshing.cpp` |
| `cmake/MatterViewer.cmake`, `cmake/tests/viewer_graph_tests.cmake` | +10/-8 | Source regex, counts 35→41 and 227/226→233/232, include dirs |
| `cmake/MatterEngine.cmake`, `MatterEngine3/tests/Makefile` | +2 | `live_terrain_session.cpp` in `live_terrain_tests` |
| `cmake/tests/editor_registration_{expected.json,census.md}` | +7 | The new world `LiveTerrainFixture` (76 worlds). The property set is unchanged because it rides in `viewer.debug` |
| `MatterEngine3/tests/live_terrain_tests.cpp` | +146 | `test_live_terrain_session` |
| `projects/world_demo/scenes/examples/LiveTerrainFixture/LiveTerrainFixture.js` | new | The fixture world |
| `docs/component-separation/live-terrain-editor-qualification{.md,/}` | new | This record, `measure_capture.py` and the §2 evidence (capture JSON/PNG, measurements, log excerpt) |

**Why `live_terrain_session.cpp` is over 50 lines.** `LiveTerrain` (.9.9)
takes a caller-owned `MemoryCache`, a `GraphicsService` and, per pump, a
`TerrainService`. Something has to own those three for the lifetime of a
world session. That owner also has to turn a world's field program into
.9.9's request policy: the same capability set and budgets that the .9.9
tests use. Finally, it has to refuse late work naming a replaced program.

Putting these into `matter_engine.cpp` would add about 80 more lines to a
14k-line file and would hide them from the test. Keeping them in one class
lets `test_live_terrain_session` drive the exact calls the editor makes.

The file contains no algorithm. It holds:
- the 16-line `SourceService` field guard and counter;
- the 6-line `options()`;
- `set_source`, which is mostly the 18-line request policy;
- `update`, which is 6 lines;
- `collision_changed`, which is 6 lines.
