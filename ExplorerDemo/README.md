# ExplorerDemo

Standalone fly-through demo for the Meadow Valley world. Consumes only the
`MatterEngine3` public API (`matter/*.h`) and links `libmatter_engine3.a`.

## Building

```bash
# Linux
make -C ExplorerDemo

# Windows (MinGW cross-compile from Linux/WSL)
make -C ExplorerDemo windows
```

## Running

Run from the `ExplorerDemo/` directory so that `cache/` resolves correctly:

```bash
cd ExplorerDemo
GALLIUM_DRIVER=d3d12 ./explorer        # Linux (d3d12 required on WSLg)
./explorer.exe                         # Windows: double-click or run from cmd
```

## Data directory resolution

The app resolves world data in this priority order:

1. `EXPLORER_DATA_DIR=<path>` — explicit override (packaged layout under `<path>/`)
2. `./WorldData/` — packaged layout in the current directory (next to the exe)
3. `../MatterEngine3/examples/world_demo/...` — dev fallback (when `./WorldData` is absent)

Packaged layout (`WorldData/`):
```
WorldData/
  schemas/        — world schema .js files
  shared-lib/     — shared DSL library .js files
  worlds/Meadow/  — Meadow world data (world.manifest)
```

## Packaging (Windows distributable zip)

```bash
bash ExplorerDemo/tools/package_explorer.sh
```

Produces `ExplorerDemo/dist/MeadowValley-Explorer-<date>.zip` containing
`explorer.exe`, `WorldData/`, and `README.txt`. First-run bake is ~2 min;
subsequent runs reuse `cache/` created next to the exe.

## Controls

| Input | Action |
|-------|--------|
| Tab | Toggle mouse capture |
| WASD | Move (horizontal) |
| Q / E | Move down / up |
| Left Shift | Speed boost (4x) |
| Mouse (captured) | Look |
| Gamepad left stick | Move |
| Gamepad right stick | Look |
| Gamepad triggers | Vertical (RT=up, LT=down) |

## Smoke test

```bash
cd ExplorerDemo
GALLIUM_DRIVER=d3d12 EXPLORER_SMOKE="secs=20,shot=/tmp/explorer_smoke.png" ./explorer
```

Expected: prints `explorer: ready` after the first BakeStarted event, runs for
the requested number of seconds, then exits 0.
