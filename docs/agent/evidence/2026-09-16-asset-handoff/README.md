# Frozen asset-authoring runtime

Delivered directory: `MatterEditor/build/asset-handoff/2026-09-16-r1`.
Start with `launch.cmd`; the asset-agent workflow and export limitations are in
[the handoff guide](../../asset-handoff.md) and the snapshot's own README.

The MSVC RelWithDebInfo editor, embedded shaders and engine JS helpers are fixed
in this separately versioned directory. Its copied world_demo project is
editable. The launcher verifies 17 runtime files and enables 512 texels/metre
for prop inspection; `-TextureDensity` overrides that authoring preset. Normal
builds do not target this directory. Keep this version unchanged when continuing
engine development; create a new handoff version when needed.

## Validation

- Five native build targets passed; 12 focused checks passed with zero Vulkan
  validation errors and no RT skips. Frozen source/binary/log evidence is in
  [the paired-feedback audit](../2026-09-16-periodic-material-feedback/final-audit.json).
- Executable imports resolve from Windows System32 without developer-runtime
  DLLs. Optional PhysX and Streamline are disabled in this build.
- The copied executable passed registration census and rendered the copied
  brick-wall scene with PATH restricted to System32. Scene enumeration and
  cache logs confirm assets come from the snapshot's project directory.
- The first cold bake finished with zero bake errors. Visual review exposed
  the engine's default 16 texels/metre as too coarse for brick inspection. The
  explicit 512-texel launcher preset yields the [accepted setup capture](walls-authoring.png)
  with visible brick/mortar and surface detail. This verifies the asset-review
  setup, not completion of the broader texture appearance or performance goals.
- [Launch results](launch-results.json) report startup/capture completion only;
  their elapsed times are not a controlled frame-time benchmark. The second
  wall run used warmed source caches. Runtime hashes were checked again after
  capture; [handoff.json](handoff.json) records the identities and outcome.

No general OBJ exporter or composed-VT/lossless texture exporter was added.
Existing tileset PNG debugging is documented with its two-channel normal,
8-bit height and cache-miss limitations. Source inspection found only specialized
hydrology/test OBJ writers.
