# Cache per-page LRU touch cells in prepared hierarchies

Each selected node previously searched gpu_pages (a sorted map) to update its
last-used epoch during every scene rebuild. Snapshot capture now records a shared
counter cell for each resident node. Main-lane selection writes that cell directly.
Pressure eviction reads the current page record's counter with unchanged ordering.
Workers transport ownership but do not read or write counter values.

Counters have independent shared ownership: eviction, page republication under
the same content hash, canceled hierarchy work and world reset cannot leave a
cached dangling address. Republishing a page creates a new counter; older snapshot
touches do not make an unrelated newer allocation look recently used. One small
counter allocation per published page is added in exchange for eliminating repeated
selected-node tree searches. No large runtime bank allocation is introduced.

Native editor build passed (/tmp/hierarchy-touch-owned-build.log). Measurement
uses the same 240-camera path and synchronous whole-scene assembly comparison mode.

## Results

The matched 240-camera movement run completed successfully. Median frame time
fell from 35.36 ms to 27.91 ms, p95 from 48.60 ms to 39.63 ms, and maximum
from 93.13 ms to 68.04 ms. These are single runs, not repeated averages.
Hierarchy bookkeeping/assembly fell from approximately 7.43 to 1.44 ms per
movement frame. No page failures, watchdogs, evictions or reservation stalls
were reported. This does not prove eviction-pressure behavior or the 10 ms goal.

Two additional reload cycles with whole-scene assembly explicitly enabled
completed with exit 0 (hierarchy-touch-reload-v1/editor.log). Both after_reload
markers appeared. This exercises cancellation and snapshot lifetime across reset.
Raw runs are in C:/tmp/matter-blas-mountain/moving-hierarchy-touch-v1 and
C:/tmp/matter-blas-mountain/hierarchy-touch-reload-v1.
