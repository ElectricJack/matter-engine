# ExplorerDemo

Standalone fly-through demo for the Meadow Valley world. Consumes only the
`MatterEngine3` public API (`matter/*.h`) and links `libmatter_engine3.a`.

## Building

```bash
make -C ExplorerDemo
```

## Running

Run from the `ExplorerDemo/` directory so that `cache/` resolves correctly:

```bash
cd ExplorerDemo
GALLIUM_DRIVER=d3d12 ./explorer
```

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
