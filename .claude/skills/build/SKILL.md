---
name: build
description: Canonical build commands for MatterEngine3, MatterEditor, and their test suites, plus the exit-code and shader-propagation gotchas. Use when asked to build, compile, or rebuild the engine/editor.
---

## Build commands (post-platform.mk: no manual TMP/TEMP needed)

```bash
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
make -C MatterEngine3                       # -> build/libmatter_engine3.a
make -C MatterEditor windows                # -> build/windows/editor.exe
make -C MatterEngine3/tests run-<target>    # headless test, e.g. run-world-definition
```

`platform.mk` (included by all three) now derives/exports TMP/TEMP itself —
the old `TMP=... TEMP=...` boilerplate is gone. It also wires ccache into
every gcc/g++ line here; verify a warm cache with `ccache -s`.

## RETOPO=0

`MatterEditor windows` builds the autoremesher_core retopo backend by
default (`RETOPO ?= 1`). Pass `RETOPO=0` for a faster build without it.

## Shader edits propagate through a plain build (verified 2026-08-14)

A change under `shaders_vk/` rebuilds via a plain `make -C MatterEngine3` /
`make -C MatterEditor windows`, no extra step: `embedded_spirv.h` is a real
prerequisite of every archive object, so touching a shader forces the `.spv`
rebuild, header regen, and recompile (confirmed with `make -n`). The
`vulkan-spirv` target regenerates just the header standalone; it is not
required for propagation — an older belief that plain make leaves stale
SPIR-V describes a since-fixed bug.

## Check by exit code, never by grepping output

Never decide a build passed by grepping stdout for "error" — passing builds
print "error" in warnings and expected-failure test output too.

## Windows link caveat

`MatterEngine3/tests` targets linking `-lGL -lX11 -ldl -lrt` fail to *link*
on Windows; compilation still succeeds. GL/raylib-free targets
(`run-world-definition`, `run-script`, `run-lod-distance`, ...) run fully.

See `docs/agent/qa-cookbook.md` recipes 1-2, 10 and `control-surface.md` §e.
