# Task 4 Report — RefineController Tile Pairing + Priority/Eviction Model

## Changes Made

### New files
- `MatterEngine3/src/refine_controller.h` — defines `span<T>`, `GraphNode`, `InstanceRef`, `TileRecord`, and `RefineController` class
- `MatterEngine3/src/refine_controller.cpp` — pure CPU implementation
- `MatterEngine3/tests/refine_controller_tests.cpp` — 8 synthetic-data test cases

### Modified files
- `MatterEngine3/tests/Makefile` — added `REFINECTL_TARGET/CPP/OBJS` under `def` flavor; added `run-refinectl` `.PHONY` target + link rule + `clean` entry

### Snapshot struct: no change required
`part_graph_snapshot::Node` already carries `module` (string) and `params_json` (canonical string) since Task 9. No modification needed. `run-asyncbake` was not run.

## Design Notes

### Canonical params dependency
`params_to_json` (part_graph.cpp) produces keys in sorted `std::map` order, numbers via `%.17g`, strings quoted, no whitespace. Integer values like `tx:5` print without a decimal point.

The `extract_str` / `extract_int` helpers rely on this exact form — they search for literal needle substrings like `"tx":` and are not general JSON parsers. This is documented with a comment in refine_controller.h and refine_controller.cpp.

### Tile pairing
Terrain nodes are grouped by a `(tx << 32 | tz)` key using `std::map<uint64_t, TileAccum>`. The `res` field distinguishes coarse from full within each group. Coarse instance hash is looked up in an `unordered_map<uint64_t, const InstanceRef*>` to populate `TileRecord::pos` (= translation + TILE_SIZE/2 per XZ) and `manifest_idx`.

### Allocations
`tiles_` is `std::vector<TileRecord>`. Per the brief, the ~2,601 element bound means std::vector is appropriate (the MemoryLib constraint targets bulk geometry buffers, not control data).

### TILE_SIZE = 10.0f
Declared as `constexpr float` in `RefineController`. Task 6 wires the real value; tests are written using matching 10.0 unit spacing.

## Test Evidence

Command: `make -C MatterEngine3/tests run-refinectl`

```
[test_tile_count]
ok tile_count
[test_full_count]
ok full_count
[test_next_nearest]
ok next_nearest
[test_mark_queued]
ok mark_queued
[test_mark_full]
ok mark_full
[test_evict_beyond]
ok evict_beyond
[test_no_coarse_instance]
ok no_coarse_instance
[test_next_all_full]
ok next_all_full
ALL PASS
```

8/8 tests pass. 0 failures. No compiler warnings.

## Self-Review

- Interfaces match the brief exactly (span, GraphNode, InstanceRef, TileRecord, RefineController methods).
- `next()` uses linear scan (O(n)); 2,601 tiles is negligible.
- `evict_beyond()` sorts farthest-first using `std::sort` descending by d².
- Non-Terrain modules are silently ignored in `build()`.
- Full-res nodes without a matching coarse instance produce a record with `pos={0,0,0}` and `manifest_idx=0`; pairing via (tx,tz) still works.
- Snapshot struct unchanged — `run-asyncbake` not required.

## Concerns

None.
