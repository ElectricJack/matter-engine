# Sector-cube frustum request priority

WorldSession sends unjittered camera planes through the coordinator's mutex-
protected anchor snapshot. Anchor position updates preserve the current view;
clear/detach clears it with the anchor. Worker tick updates the desired cube set,
then classifies it against six planes once. Request selection ranks visible
cubes before offscreen candidates and retains distance/hole ranking within each
class. Existing seam holds, residency, acknowledgements and generations remain.
Rotation updates priority without restarting the streamer. Inflight work is not
yet reprioritized/cancelled, nor are decode/upload queues reordered by visibility.

The new policy is for volumetric cubes. StreamMountain already enables them;
audit now explicitly sets MATTER_VOLUMETRIC_SECTORS=1 to override inherited
rollback settings. No unbounded-column visibility approximation is implemented.
Legacy selector paths remain but are outside this goal's acceptance architecture.

Tests add visible distant cube vs nearby offscreen priority, rotated view without
anchor movement, and no-view nearest-first behavior. Existing coordinator suite
passed; cube selector suite and editor scene validation recorded below when done.
Editor build /tmp/frustum-cube-editor-build.log passed. Global readiness audit
still needs a visible-target readiness predicate before testing sub-second
acceptance. First view currently arrives during render; startup work issued
before then remains a further scheduling gap.

Cube selector suite ALL PASS (/tmp/frustum-cube-tests.log), including existing
six-face balance checks (928 adjacent pairs, worst level gap 1). Coordinator
suite ALL PASS (/tmp/frustum-final-coordinator-tests.log). Python syntax passed.

Strict scene audit C:/tmp/matter-blas-mountain/sector-frustum-v1:

```json
{
  "valid": true,
  "readiness_phases": [
    {
      "ready_seconds": 10.07599329999357,
      "confirmed_seconds": 25.12325079999573,
      "camera_turn_seconds": null
    }
  ],
  "geometry_coverage": [
    0,
    0,
    0
  ],
  "failures": [],
  "compilations": 0
}
```

Full-scene cache/coverage validation passed; visible-set latency and camera-turn
latency remain unmeasured, so the one-second requirement remains open.
