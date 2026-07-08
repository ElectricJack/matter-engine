## Task 5 Report: Public API header `autoremesher/remesh.h`

**Status:** DONE

**Commit SHA:** 5c535b2

**Test summary:** `g++ -std=c++17 -include remesh.h -c -x c++ /dev/null -o /dev/null` — no output, exit code 0.

**Details:**
- Created `Libraries/autoremesher_core/include/autoremesher/remesh.h` verbatim from the brief.
- Exposes `autoremesher::Mesh`, `autoremesher::Options`, `autoremesher::Result`, `autoremesher::remesh()`, and `autoremesher::AUTOREMESHER_CORE_VERSION` with all field defaults per the plan (`target_ratio=1.0f`, `iterations=3`, `seed=0`, `timeout_seconds=60`, `threads=1`).
- Header includes `#pragma once` and is a self-contained C++17 compile unit.
- No divergences from the brief. Header contents match the spec verbatim.
