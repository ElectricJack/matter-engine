# GI lightmap bake baseline — 2026-09-15

Tool: `matter bake gi` (docs/bake-gi.md), commit `feat(gi): bake sun, sky and
bounce lighting into per-instance lightmaps` on `aq/eager-bridge`. Native
MSVC RelWithDebInfo `matter.exe` built by `tools/build-windows-from-wsl.sh`,
launched from WSL through interop with the repository root as the working
directory. Worlds were already baked by the editor (part cache warm), so
"load" is `LocalProvider::connect()` on a warm part cache except where noted.

Host: AMD Ryzen 9 5900X (12 cores / 24 threads), 64 GB, Windows 11; the repo
lives on a mounted `D:` drive. The bake is CPU-only (docs/bake-gi.md
"Limitations"); the dev GPU is not used.

## Scenes at the defaults (64 spp, 2 bounces, 8 texels/m)

| scene | instances | covered texels | atlas texels | rays | trace ms (sum) | bake ms | load ms | write ms | wall s | output |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| RockGallery, cold store | 13 | 7,967 | 66,304 | 699,711 | 90 | 249 | 974 | 99 | 4.17 | 52 files, 0.8 MB |
| RockGallery, warm store | 13 | 7,967 | 66,304 | — | 0 | 5 | 987 | 103 | 1.18 | same |
| CastleMaterials, cold store, cold provider | 16 | 12,872 | 152,640 | 1,967,162 | 198 | 330 | 35,101 | 144 | 35.6 | 65 files, 4.9 MB |
| CastleMaterials, warm store | 16 | 12,872 | 152,640 | — | 0 | 98 | 1,084 | 142 | 1.33 | same |

"bake ms" covers atlas build, rasterization, tracing, the three post passes
and the store round trip for every instance; "trace ms (sum)" is the traced
time summed over instances. The 35 s CastleMaterials load is the provider
re-flattening after the editor's cache state changed, not the bake; the warm
line shows the steady state. Wall time includes process start-up and the
world connect.

## Heavy configuration

`matter bake gi CastleMaterials --texel-density 32 --samples 256 --bounces 3 --no-cache`

| instances | covered texels | atlas texels | rays | trace ms (sum) | bake ms | wall s | rays / s (traced) |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 16 | 162,629 | 655,872 | 105,039,340 | 3,761 | 4,100 | 5.21 | ~28 M |

Per instance the ground plane (512x525, 99,936 covered texels) traced 52.8 M
rays in 1.13 s. At this throughput a scene with 1 M covered texels at 256 spp
and 3 bounces (~1 G rays) is under a minute, and the default configuration
(64 spp, 2 bounces) is about 6x cheaper per texel — well inside the
"minutes per scene" design point.

## Output size

The compact shelf pack matters: the same RockGallery bake with the renderer's
page-aligned chart packer (charts rounded to 128-texel pages for the VT pool)
produced 5.67 M atlas texels for 7,967 covered ones and 49.3 MB of files;
the compact pack produces 66,304 atlas texels and 0.8 MB. The 16-bit PNG is
stored (uncompressed deflate) and is the largest file per instance.

## Headless fixture (gi_bake_tests)

Two-room fixture at 48 spp, 2 bounces, 4 texels/m, 2 threads:

| host | ground 256x168 (26,240 texels, 2.76 M rays) | room 128x60 (3,548 texels, 0.72 M rays) |
| --- | ---: | ---: |
| WSL2 Ubuntu, 2 vCPUs, g++ 13 -O2 | 773 ms | 513 ms |
| Windows native MSVC RelWithDebInfo | 123 ms | 83 ms |

Both hosts produce identical texels (courtyard 1.687/1.647/1.588, seam step
0.0197 on a 2.341 level), which is the determinism contract.

## Method notes

- Timings come from the tool's own census (`std::chrono::steady_clock`
  around each phase) and `/usr/bin/time` around the process.
- Single run per row; no clock locking. Treat the numbers as an order of
  magnitude for this host, not a regression threshold.
- Raw logs: `MatterEditor/build/gi-out/*/manifest.json` carries the per-
  instance `trace_ms`, `rays` and `covered_texels` for every run.
