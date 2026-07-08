#!/usr/bin/env bash
# grep_gate.sh — dependency rule enforcement for app projects.
# App projects (MatterViewer) may include only:
#   matter/*.h           — the public kernel API
#   raylib.h / rlgl.h / raymath.h  — raylib public headers
#   imgui*.h / rlImGui.h — imgui backends
#   ui.h                 — the viewer's own app header
#   resource_dir.h       — resource-path helper (vendored)
#   GLFW/*.h             — GLFW (imgui_impl_glfw.cpp needs it)
# Any #include "..." not matching those patterns is an engine internal
# leaking into app code — a regression from the kernel-extraction split.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
fail=0
for app in MatterViewer; do
  hits=$(grep -rnE '#include\s+"' "$ROOT/$app" --include='*.cpp' --include='*.h' \
    | grep -vE '"(matter/|raylib|rlgl|raymath|imgui|rlImGui|ui\.h"|resource_dir|GLFW/)' || true)
  if [ -n "$hits" ]; then
    echo "GREP-GATE FAIL: $app includes engine internals:"; echo "$hits"; fail=1
  fi
done
[ $fail -eq 0 ] && echo "grep-gate: clean"
exit $fail
