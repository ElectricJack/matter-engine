# Task 4: PathRecorder + Checkpoint A — Report

## Summary

Successfully implemented PathRecorder observer class (Task 4) and passed Checkpoint A gate. The PathRecorder records particle trajectories into an append-only PathSet with per-vertex channel data, min-segment decimation, automatic death-closing, and provides the path_end_dir() helper function.

## Implementation

### Files Modified/Created

1. **ParticleFlowLib/include/particle_flow.h**
   - Added PathRecorder class declaration (public ITickObserver)
   - Added path_end_dir() free function declaration
   - PathRecorder constructor takes min_segment float and channel_names vector
   - Private Track struct for per-particle trajectory tracking
   - append_vertex() helper for recording positions and attributes

2. **ParticleFlowLib/src/pf_path_recorder.cpp** (new file)
   - PathRecorder constructor initializes min_seg_ and channel_names
   - on_tick() implementation:
     - Ascending slot iteration for determinism
     - Tracks known particles by id; lazily creates paths on spawn
     - Appends vertices when particle moves >= min_segment from last recorded position
     - On death, appends final position if moved, then closes path
   - append_vertex() pushes position xyz and all attribute channels to path
   - path_end_dir() computes normalized direction of final segment (or {0,0,0} for single-vertex)

3. **ParticleFlowLib/tests/pf_tests.cpp**
   - Added pathsets_equal() helper for comparing PathSet objects
   - Added test_path_recorder() test:
     - Verifies N+M ticks produce identical results to (N); (M) runs
     - Validates channel_names match config.attributes
     - Verifies all paths have >= 1 vertex
     - Checks min-segment decimation (consecutive vertices >= 0.02 apart)
     - Validates closed paths exist (max_age deaths)
     - Tests path_end_dir() returns unit direction
   - Added test_pathset_append_only() test:
     - Verifies monotonic accretion: new vertices appended, old never move
     - Snapshots PathSet after 100 ticks, runs 100 more, verifies prefix unchanged

## Test Results

### Step 2: Initial Failure (Expected)
Test compilation failed with "PathRecorder not declared" — correct TDD flow.

### Step 4: Implementation Success
All tests compiled and ran successfully:
```
pf_tests:
  rng determinism OK
  v3 math OK
  spatial hash OK (500 pts)
  spatial hash no-duplicates OK (729 pts, 50 queries)
  sim determinism + incremental OK (150 alive, 16749 deposited)
  gravity parabola OK (y=-20.0900 expect -20.0900)
  turn clamp OK
  emission/cap/age/reuse OK
  adhere OK (x=0.283)
  separate OK (0.200 -> 3.064)
  attract consume/kill OK
  surface normal OK
  curl OK
  path recorder OK (100 paths, 40 closed)
  pathset append-only OK
pf_tests: ALL OK
```

### Checkpoint A: Full Build + Test Gate
```bash
make -C ParticleFlowLib clean && make -C ParticleFlowLib && make -C ParticleFlowLib test
```

Result:
- **Library build**: `libparticleflow.a` built clean (2.3M, no warnings beyond repo norms)
- **Test build**: Compiled with -fsanitize=address,undefined (single unused-param warning in existing test)
- **Test run**: ALL 15 TESTS PASS (13 existing + 2 new PathRecorder tests)
- **ASan/UBSan**: No sanitizer reports; clean execution

## Implementation Notes

1. **Determinism Preserved**: On_tick iterates slots in ascending order, maintaining determinism requirement. Multiple runs with same seed+config produce bit-identical paths.

2. **Incremental Equivalence**: Verified by test — run(N) followed by run(M) produces identical PathSet as single run(N+M). This works because:
   - Track state (by_id_, known_) persists across run() calls
   - Append-only PathSet ensures no rewrites
   - Deaths recorded once per particle

3. **Min-Segment Decimation**: Works by tracking last_recorded position (t.last) and only appending when distance >= min_seg_. Special case: spawn always records first vertex, deaths always record final position if moved.

4. **Path Closing**: On death, final position appended (if > 1e-6 from last recorded), then path.closed = true. Guard against double-close prevents issues if particle death somehow fires twice.

5. **Channel Consistency**: Each new path initializes channels.resize(channel_count), then append_vertex pushes per-channel values. Verified by test assertions.

## Deviation Analysis

No deviations from brief. All interfaces match exactly:
- `PathRecorder(float min_segment, const std::vector<std::string>& channel_names)` ✓
- `on_tick(const Sim& s, uint32_t tick) override` ✓
- `const PathSet& paths() const` ✓
- `V3 path_end_dir(const PathSet::Path& p)` free function ✓

## Self-Review

Strengths:
- Clean separation: PathRecorder fully encapsulated in observer pattern
- No coupling to Sim implementation; uses only public accessors
- Deterministic by design (ascending slot order, instance-owned state)
- Append-only PathSet enforces monotonic accretion structurally
- Comprehensive test coverage validates all requirements

Potential concerns:
- Dense id tracking (by_id_ and known_ vectors resize on max id seen) — acceptable for particle counts up to ~100k
- No deduplication of spawn/death positions if they coincide — test shows this is not an issue in practice
- Single warning about unused parameter in existing test_spatial_hash_vs_brute_force (pre-existing, not from this task)

All constraints satisfied:
- C++17, -Wall -Wextra ✓
- ASan/UBSan clean ✓
- No globals/static mutable state ✓
- Determinism preserved ✓
- Append-only guarantee ✓
- Incremental equivalence ✓
- Checkpoint A gate: PASS ✓

## Commit

```
d5c8064 feat(particleflow): PathRecorder — append-only PathSet with decimation, death-closing, end directions
```

Stages: ParticleFlowLib/include/particle_flow.h, ParticleFlowLib/src/pf_path_recorder.cpp, ParticleFlowLib/tests/pf_tests.cpp
