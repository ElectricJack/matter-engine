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

The small glowing source meshes are a separate
`LocalLightRtGlowProxy` Part. It calls `rayTraced(false)`; analytic lights own
the source power, while the iron lamp bodies remain in the RT-visible physical
fixture. `localLightRtGalleryRoots(false)` omits only that cosmetic Part, and
`localLightRtGalleryLights(false)` produces the exact zero-local-light control.
The Node fixture check asserts both contracts.

## Native capture setup

Build with the canonical Windows toolchain first:

```bash
./tools/build-windows-from-wsl.sh RelWithDebInfo matter_editor
```

Then launch from `MatterEditor/` with an append-only Windows command file. Use
the same process for every A/B image so camera, exposure, and accumulated state
remain fixed. The `history_reset` and settle wait after each property change are
intentional.

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
cam 5.4 2.9 6.4 1.3 1.25 0.3
set render.gi.enabled true
set render.gi.diffuse_multiplier 0
history_reset
wait_frames 48
shot C:/tmp/local-light-rt/corner-diffuse-gi-zero.png
set render.gi.diffuse_multiplier 1
history_reset
wait_frames 64
shot C:/tmp/local-light-rt/corner-diffuse-gi-one.png
```

Expected: direct illumination that is visible elsewhere does not change when
the diffuse multiplier reaches zero. The pale sphere/wall around the solid
return gains a red-orange component only in the second image. A bright direct
spot on that receiver is a visibility failure.

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
cam 14.0 2.85 6.2 13.5 1.45 0.1
set render.gi.enabled false
history_reset
wait_frames 48
shot C:/tmp/local-light-rt/materials-gi-off.png
set render.gi.enabled true
history_reset
wait_frames 64
shot C:/tmp/local-light-rt/materials-gi-on.png
```

Expected: the gold sphere retains its warm local-direct GGX highlight in both
images. With GI enabled, reflection/transmission hits can also respond to the
off-camera green source/card. The cool point source at z=-3.2 reaches the pale
target through the closed glass sphere using the renderer's transmission
visibility semantics; it must not be treated as an opaque wall or ignored.

### 5. Cosmetic proxy toggle

```text
set render.gi.enabled true
set render.lighting.emission_multiplier 1
history_reset
wait_frames 48
shot C:/tmp/local-light-rt/proxy-visible.png
set render.lighting.emission_multiplier 0
history_reset
wait_frames 48
shot C:/tmp/local-light-rt/proxy-hidden.png
```

Expected: only the tiny visible glow dots disappear. Local direct, local-light
bounce, and physical iron lamp bodies remain stable because source energy is
analytic and the glow Part never entered the TLAS. This live emission control
also affects unrelated authored emissive materials; this scene intentionally
contains none.

### 6. True zero-light source control

The zero-list state is an authoring fixture rather than a live render property:
change only the two default factory calls at the bottom of
`LocalLightRtGallery.js` to `localLightRtGalleryRoots(false)` and
`localLightRtGalleryLights(false)`, reload, and capture the overview camera
`cam 23 11 27 0 1.7 0`. Expected: no local-light illumination, no glow proxies,
and no bake/descriptor/validation error. Restore both arguments to `true`
before committing or taking the other captures. The automated fixture check
exercises the same zero-list factory without editing the source.

Finite-radius sources use four deterministic visibility samples, producing a
stable bounded-cost penumbra. Do not infer a performance target from this
six-light correctness fixture; use the 289-point `LocalLightGallery` for
candidate distribution and measured GPU/frame timing.

## Script checks

From the repository root:

```bash
node --check projects/world_demo/scenes/LocalLightRtGallery/LocalLightRtGallery.js
node projects/world_demo/scenes/LocalLightRtGallery/fixture_tests.mjs
```
