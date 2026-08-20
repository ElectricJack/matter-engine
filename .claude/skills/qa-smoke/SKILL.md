---
name: qa-smoke
description: Run the Vulkan smoke gate (12 fault/RT/VT modes) or a single MATTER_VK_SMOKE_MODE case against the editor's Vulkan build. Use when asked to run the smoke tests, the Vulkan gate, or verify a Vulkan/VT change didn't break validation.
---

## Full gate (all 12 modes)

```bash
make -C MatterEngine3/tests vulkan-smoke
```

Delegates to `make -C MatterEditor vulkan-smoke`, which builds the smoke exes
and runs `MatterEditor/tools/smoke_vulkan_faults.ps1` — each mode is its own
process, 30s timeout by default (`rt` 90s, `rt-transmission` 45s). Every mode
must print `validation errors: 0` and `ALL PASS`, exit 0, or the gate fails.

## The 12 modes

`streamline-missing-instance-proxy`, `streamline-missing-device-proxy`, `rt`,
`rt-transmission`, `rt-disabled`, `rt-unavailable`, `animation-skin`, `vt`,
`vt-surfaces`, `vt-rt`, `vt-enrich`, `vt-enrich-nort`.

## Single mode (faster iteration)

```bash
cd MatterEditor
MATTER_VK_SMOKE_MODE=vt-enrich ./build/windows/vulkan_smoke_tests.exe
```

The exe supports more modes than the gate exercises (`cull`, `tileset`,
`transform`, `outlive-unproven`, `retention-fault-*`, ...) — grep
`vulkan_smoke_tests.cpp` for `std::string(smoke_mode) ==` for the full set.

## If you need to (re)build the editor first

```bash
make -C MatterEditor windows RETOPO=0
```

`RETOPO=0` skips the autoremesher_core retopo backend (`RETOPO ?= 1` default
in `MatterEditor/Makefile`); the smoke tests don't exercise it. Never build
while an existing `editor.exe`/smoke exe is running (file lock).

See `docs/agent/qa-cookbook.md` recipe 6 for the full mode rationale and
`docs/agent/control-surface.md` for `MATTER_VK_*` env vars.
