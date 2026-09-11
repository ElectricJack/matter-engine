# LocalLightRtGallery

`LocalLightRtGallery` is the compact native-ray-tracing acceptance scene for
analytic local-light visibility and secondary-hit lighting. Authored sun and
sky are black. Five shadow-casting finite-range point lights and one spot light
are therefore the only physical illumination.

The scene is laid out west to east:

- Two roofed rooms share one 2.2 m doorway. A warm point source in the west
  room must illuminate the east floor only through that opening; the solid
  partition returns and outer walls must remain shadowed.
- An L-return separates a neutral point source and saturated terracotta card
  from a pale receiver. The source-to-receiver segment is blocked. Red-orange
  radiance around the corner is consequently a GI result, not direct leakage.
- A blue spot, two fins, polished gold, a closed clear-glass volume, and a pale
  transmission target occupy the east station. A green-lit card outside the
  tight material camera checks local-light lookup at off-screen secondary hits.
- A thin `ForestFloor` detail slab beside the east station is the closed-world
  POM witness. Its warm point-light path is unobstructed, so native RT going
  dark while raster remains lit is a POM-origin self-intersection failure.

The small glowing source meshes are a separate
`LocalLightRtGlowProxy` Part. It calls `rayTraced(false)`; analytic lights own
the source power, while the iron lamp bodies remain in the RT-visible physical
fixture. `localLightRtGalleryRoots(false)` omits only that cosmetic Part, and
`localLightRtGalleryLights(false)` produces the exact zero-local-light control.
The Node fixture check asserts both contracts. It also proves geometrically
that the neutral source cannot see either outlined pale receiver while the
terracotta card does see them around the positive-z end of the return.

## Native capture setup

Build with the canonical Windows toolchain first:

```bash
./tools/build-windows-from-wsl.sh RelWithDebInfo matter_editor
```

For the reopened GI/material/proxy evidence, run the checked-in driver from a
native PowerShell at the repository root:

```powershell
& .\projects\world_demo\scenes\LocalLightRtGallery\capture-reopen.ps1 `
    -OutputDir C:\tmp\local-light-rt-reopen
```

It launches `MatterEngine3/tools/drive.py`, removes stale expected shots through
that driver, and writes the six PNG/`.done` pairs plus `run/log.txt` below the
output directory. Each comparison uses one camera command, explicit exposure
and GI/emission baselines, an explicit history reset, and an equal 96-frame
presented wait on both sides (followed by `shot`'s identical three-frame
settle). It fails if native RT or a property is unavailable, a bake/idle wait
times out, or the log reports a bake or Vulkan validation error. The Node
fixture test pins the exact command ordering so the two sides cannot silently
drift.

For the doorway/spot cases or the authoring-time zero-light reload, launch from
`MatterEditor/` with an append-only Windows command file. Use the same process
for every A/B image so camera, exposure, and accumulated state remain fixed.
The `history_reset` and settle wait after each property change are intentional.

```bash
mkdir -p /mnt/c/tmp/local-light-rt
cd MatterEditor
WSLENV=MATTER_WORLD:MATTER_CMD_FIFO:TMP:TEMP \
MATTER_WORLD=LocalLightRtGallery \
MATTER_CMD_FIFO='C:/tmp/local-light-rt/commands.txt' \
TMP='C:/Users/<you>/AppData/Local/Temp' \
TEMP='C:/Users/<you>/AppData/Local/Temp' \
./build/windows-msvc/editor.exe
```

Append the following command blocks to
`/mnt/c/tmp/local-light-rt/commands.txt`. Every `shot` produces a `.done`
sidecar. A failed `set` or unavailable `native_rt` is a failed acceptance run,
not evidence to ignore.

## Exact capture cases

### 1. Doorway visibility and GI-independent direct

```text
render_path native_rt
cam -6.0 2.25 3.8 -11.1 1.65 0.0
set render.gi.enabled false
history_reset
wait_frames 48
shot C:/tmp/local-light-rt/doorway-rt-gi-off.png
set render.gi.enabled true
history_reset
wait_frames 48
shot C:/tmp/local-light-rt/doorway-rt-gi-on.png
```

Expected: the doorway-shaped warm direct patch remains present in both images;
the partition returns, ceiling, and room exterior block the source. GI may add
indirect fill but disabling it must not disable local direct.

### 2. Colored bounce around the corner

```text
render_path native_rt
set render.gi.diffuse_multiplier 1
set render.lighting.emission_multiplier 1
set render.lighting.exposure_ev 0
cam 5.8 3.2 7.4 1.5 1.30 1.05
set render.gi.enabled false
history_reset
wait_frames 96
shot C:/tmp/local-light-rt/corner-gi-off.png
set render.gi.enabled true
history_reset
wait_frames 96
shot C:/tmp/local-light-rt/corner-gi-on.png
```

Expected: primary direct illumination visible elsewhere remains in both images.
The pale sphere and the pale panel inside the dark three-sided outline gain a
red-orange component only with GI on. Their direct path to the neutral source
crosses the solid return; the enlarged terracotta card's path to them clears its
positive-z end. A bright neutral direct spot inside the outline is a visibility
failure. There is deliberately no second `cam` command inside the pair.

### 3. Spot cone and traced occlusion

```text
cam 12.1 5.2 10.2 11.7 1.0 -0.2
set render.gi.enabled false
history_reset
wait_frames 48
shot C:/tmp/local-light-rt/spot-native-rt.png
render_path raster
wait_frames 24
shot C:/tmp/local-light-rt/spot-raster.png
```

Expected: both paths agree on the blue finite-range cone and its outer falloff.
Native RT adds the two fin shadows; baseline raster may remain unshadowed. The
spot must not illuminate behind the apex or beyond its ten-metre range.

### 4. Gold, glass, and off-screen secondary lighting

```text
render_path native_rt
set render.gi.diffuse_multiplier 1
set render.lighting.emission_multiplier 1
set render.lighting.exposure_ev 0
cam 14.0 2.85 6.2 13.5 1.45 0.1
set render.gi.enabled false
history_reset
wait_frames 96
shot C:/tmp/local-light-rt/materials-gi-off.png
set render.gi.enabled true
history_reset
wait_frames 96
shot C:/tmp/local-light-rt/materials-gi-on.png
```

Expected: the gold sphere retains its warm local-direct GGX highlight in both
images. With GI enabled, reflection/transmission hits can also respond to the
off-camera green source/card. The cool point source at z=-3.2 reaches the pale
target through the closed glass sphere using the renderer's transmission
visibility semantics; it must not be treated as an opaque wall or ignored.

### 5. Cosmetic proxy toggle

```text
render_path native_rt
set render.gi.diffuse_multiplier 1
set render.lighting.exposure_ev 0
cam 12.8 3.1 5.5 11.5 1.8 1.0
set render.gi.enabled true
set render.lighting.emission_multiplier 1
history_reset
wait_frames 96
shot C:/tmp/local-light-rt/proxy-visible.png
set render.lighting.emission_multiplier 0
history_reset
wait_frames 96
shot C:/tmp/local-light-rt/proxy-hidden.png
set render.lighting.emission_multiplier 1
```

Expected: only the tiny visible glow dots disappear. Local direct, local-light
bounce, and physical iron lamp bodies remain stable because source energy is
analytic and the glow Part never entered the TLAS. This live emission control
also affects unrelated authored emissive materials; this scene intentionally
contains none.

### 6. Closed-world POM local-direct origin

```text
render_path native_rt
set render.gi.enabled false
set render.pom.enabled true
cam 2.8 0.75 3.4 7.3 0.20 3.4
history_reset
wait_frames 96
shot C:/tmp/local-light-rt/pom-native-rt.png
render_path raster
history_reset
wait_frames 96
shot C:/tmp/local-light-rt/pom-raster.png
```

Expected: both fixed-camera shots retain warm illumination over the visibly
parallaxed relief. Native RT may add traced shadows/noise, but the slab must not
self-shadow wholesale. The fixture is a closed Part world with no streamed
terrain or sector-cache dependency.

### 7. True zero-light source control

The zero-list state is an authoring fixture rather than a live render property.
Keep the raw-FIFO editor running with the normal defaults and first append:

```text
render_path native_rt
set render.gi.diffuse_multiplier 1
set render.lighting.emission_multiplier 1
set render.lighting.exposure_ev 0
set render.gi.enabled true
cam 23 11 27 0 1.7 0
history_reset
wait_frames 96
shot C:/tmp/local-light-rt/zero-before-reload.png
```

Wait for `zero-before-reload.png.done`. Change only the two default factory
calls at the bottom of `LocalLightRtGallery.js` to
`localLightRtGalleryRoots(false)` and `localLightRtGalleryLights(false)`, then
append this block to the same command file:

```text
reload
wait_event bake.finished 900
wait_idle 2 120
render_path native_rt
set render.gi.diffuse_multiplier 1
set render.lighting.emission_multiplier 1
set render.lighting.exposure_ev 0
set render.gi.enabled true
cam 23 11 27 0 1.7 0
history_reset
wait_frames 96
shot C:/tmp/local-light-rt/zero-after-reload.png
quit
```

Expected: the first shot has the authored local illumination and glow proxies;
the second has neither, with no bake, descriptor, or validation error. Restore
both arguments to `true` after the `.done` sidecar appears and before committing
or taking other captures. The automated fixture check exercises the same
zero-list factories without editing the source.

Finite-radius sources use four deterministic visibility samples, producing a
stable bounded-cost penumbra. Do not infer a performance target from this
six-light correctness fixture; use the 289-point `LocalLightGallery` for
candidate distribution and measured GPU/frame timing.

## Script checks

From the repository root:

```bash
node --check projects/world_demo/scenes/LocalLightRtGallery/LocalLightRtGallery.js
node --check projects/world_demo/scenes/LocalLightRtGallery/objects/LocalLightRtGalleryFixture.js
node --check projects/world_demo/scenes/LocalLightRtGallery/objects/LocalLightRtGlowProxy.js
node --check projects/world_demo/scenes/LocalLightRtGallery/fixture_tests.mjs
node projects/world_demo/scenes/LocalLightRtGallery/fixture_tests.mjs
```
