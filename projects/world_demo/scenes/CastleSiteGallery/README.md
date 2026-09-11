# Connected castle scenes

The shared castle system provides four scenes:

| Scene | Composition |
| --- | --- |
| `CastleClusteredCourt` | Compact keep, hall, chapel and service range around an inner court |
| `CastleAngledBailey` | Broad bailey enclosed by wings and passages on several axes |
| `CastleBentPalace` | Successive angled hall ranges and two courts |
| `CastleSiteGallery` | All three designs arranged side by side |

Prepare reproducible capture cameras from the actual compiled wing frames,
fixtures and collision geometry. From the repository root in WSL, with Node.js,
NumPy and SciPy available:

```bash
python3 tools/prepare_castle_site_captures.py --output-dir /mnt/d/tmp/castle-final-captures
```

The tool prepares files without launching the editor. It checks rotated boxes
and convex hulls, eye clearance, near-view rays, real floor support, whole-site
framing, and furniture/glass/chandelier focal points against the 45-degree camera
frustum. It rejects source changes during preparation. The JSON manifest records
these CPU checks; rendered screenshots still require visual review. Regenerate
after changing the layout or geometry. Fine stone chips and roof shingles are
outside the collision envelopes used by these checks.

Each individual scene receives five paired raster/RT views: exterior, courtyard
at 1.65 m eye height, ground-floor hall, upper hall gallery, and chapel altar with
gold fixtures and stained glass. The gallery adds one paired overview, for 32
screenshots total at 1920×1080. Interior pairs use identical reduced sun/sky
multipliers to make authored local lighting easier to inspect. Raster settles
60 frames and RT settles 180 frames after each switch.

Build the native editor first using the repository's canonical build wrapper.
Then run the generated `D:\tmp\castle-final-captures\launch-native.cmd` from a
native Windows shell. It contains commands for this checkout's exact path and
stops if any capture fails. To run a scene independently, from the repository
root in native Windows PowerShell:

```powershell
py -3 tools/castle_scene_capture.py --world CastleClusteredCourt --timeline D:/tmp/castle-final-captures/CastleClusteredCourt/timeline.txt --out-dir D:/tmp/castle-final-captures/CastleClusteredCourt --timeout 7200 --env MATTER_WINDOW_WIDTH=1920 --env MATTER_WINDOW_HEIGHT=1080
py -3 tools/castle_scene_capture.py --world CastleAngledBailey --timeline D:/tmp/castle-final-captures/CastleAngledBailey/timeline.txt --out-dir D:/tmp/castle-final-captures/CastleAngledBailey --timeout 7200 --env MATTER_WINDOW_WIDTH=1920 --env MATTER_WINDOW_HEIGHT=1080
py -3 tools/castle_scene_capture.py --world CastleBentPalace --timeline D:/tmp/castle-final-captures/CastleBentPalace/timeline.txt --out-dir D:/tmp/castle-final-captures/CastleBentPalace --timeout 7200 --env MATTER_WINDOW_WIDTH=1920 --env MATTER_WINDOW_HEIGHT=1080
py -3 tools/castle_scene_capture.py --world CastleSiteGallery --timeline D:/tmp/castle-final-captures/CastleSiteGallery/timeline.txt --out-dir D:/tmp/castle-final-captures/CastleSiteGallery --timeout 7200 --env MATTER_WINDOW_WIDTH=1920 --env MATTER_WINDOW_HEIGHT=1080
```

The capture helper waits for final geometry publication and idle acknowledgment
before submitting camera commands. Timelines intentionally contain no
`wait_event` or `wait_idle`; the helper owns those barriers. A successful run
writes PNGs, `.done` markers, the editor log and `capture.json`. Keep the manifest
with those receipts when reviewing results. See
[`control-surface.md`](../../../../docs/agent/control-surface.md) for FIFO grammar.
