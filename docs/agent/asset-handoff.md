# Frozen build for asset authoring

Current handoff: **2026-09-16-r2**, with static mesh/material export.
The earlier **r1** remains unchanged for sessions already pinned to it.

Stable launch entry point:

[`MatterEditor/build/asset-handoff/launch.cmd`](../../MatterEditor/build/asset-handoff/launch.cmd)

This launcher reads `current.json` and opens **r2**. Its PowerShell equivalent
accepts the same scene/automation options; `launch.ps1 -VerifyOnly` checks the
runtime without starting an editor. Agents should read `current.json` to locate
the editable project. A launch script inside the **r1** directory continues to
open r1, which lacks asset export.

Windows directory:

`D:\Shared With Desktop\AI\matter-engine-cpp\MatterEditor\build\asset-handoff\2026-09-16-r2`

Double-click **launch.cmd**. The default scene is **VillaDoricColumnStudy**, with
the marble pilot and editor panes visible. For walls, run
`launch.ps1 -World ClayBrickMaze` or `-World ClayBrickWallSurfaceProof`.
F11 toggles presentation mode.

## Export

Open a part in **Bake Lab → Workbench**, finish its bake, then use **Export asset**
→ **Export OBJ + GLB + textures** with a new output directory. The Workbench
exports its current parameters; use the agent command below for an exact scene
root variant, including scene-local pilot classes.

```json
{"source":"world","module":"VillaDoricColumnPilot","lod":0,"directory":"D:/asset-exports/column-01"}
```

Send this as the arguments of `asset.export` through the included
`tools/matter_agent.py`. Existing folders are refused. The output contains
OBJ/MTL, GLB, conventional base-color/normal/ORM images, separate channel maps,
optional clearcoat, 16-bit height and a manifest. **GLB is the preferred portable
PBR preview.** Height is separate; standard viewers do not reproduce Matter POM.
[Export usage and limits](asset-export.md) cover normal conventions, legacy height
datums, unsupported features, root hashes and memory limits.

## Asset-agent workspace

Edit this snapshot's **projects/world_demo/** copy. It includes the r1 agent's
pilot source additions; `snapshot.json` records their hashes. The stable-handoff
promotion rechecked all 11 additions against the current r1 workspace; all are
already identical in r2. The executable,
embedded shaders, engine JS helpers and automation tools are fixed. Ordinary
engine rebuilds do not modify this handoff. Copy authored source/assets back to
the main checkout when ready, preserving concurrent changes and excluding caches.

The launcher verifies runtime hashes, clears inherited `MATTER_*` overrides and
sets prop density to 512 texels/metre (`-TextureDensity 256` is a lighter preview).
First bakes start cold. Project edits are expected and excluded from runtime
verification. Full instructions are in the snapshot's `README.md`.

## Validation and scope

MSVC RelWithDebInfo development snapshot; Vulkan/RT and autoremesher enabled,
optional PhysX and Streamline disabled. This is a frozen working-tree build,
separate from formal clean-tree releases. Export covers static opaque meshes and
PBR materials. Unsupported animation/voxel/terrain/transmissive inputs return
explicit errors.

[Export validation evidence](evidence/2026-09-16-asset-export/README.md) records native
checks, real column/brick/assembly exports, independent glTF validation and OBJ/GLB
previews. Runtime hashes and packaged startup/export results are in the snapshot's
`snapshot.json` and `validation/` directory. The broader texturing and performance
goals remain active.

For the original snapshot's evidence, see
[the r1 handoff](evidence/2026-09-16-asset-handoff/README.md).
