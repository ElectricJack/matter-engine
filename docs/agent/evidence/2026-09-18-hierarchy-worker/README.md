# Background hierarchy descriptor preparation

Default raster replacement path uses a bounded64-item AsyncStagePipeline to build
page indices, roots, dense CPU nodes and GPU node descriptors. Worker input owns
an immutable ResidentHierarchy and copies ready renderer part IDs; it does not
access mutable residency or renderer tables. ResidentHierarchy resource pins
retain the old and pending page allocations until publication/retirement.

Main lane retains the prior hierarchy while a replacement prepares. Completion
matches asset identity and lease; per-asset revision tracks publications affecting
either the displayed hierarchy or the pending snapshot frontier. A completed older
revision may display valid older detail but remains dirty for follow-up preparation.
This prevents continuous streaming from starving publication without losing newer
page arrivals. Reset cancels pipeline generations. Detach ignores stale leases.

Initial coverage, non-raster mode and pressure rebuilds remain synchronous.
Residency snapshot capture/ownership adoption and whole-scene packing/encoding
also remain on main. This is the first threaded conversion, not completion of
all CPU candidate migration. MATTER_GEOMETRY_HIERARCHY_ASYNC=0 uses synchronous
preparation for comparison. hierarchy_worker reports actual background build time;
snapshot now measures capture/submission or synchronous initial work.

Native MSVC editor build passed. Moving test uses the same camera path and2GiB
reservation profile, isolated from other builds/GPU tests.

Moving run completed exit0. Worker telemetry confirms13,608 preparations,
252.282ms aggregate worker time. Median movement interval33.994ms, p95 50.420ms,
max77.133ms versus40.032/51.953/147.864ms previously. One run and warm-cache
variation limit attribution; the removed work is a small portion of total runtime.
No page failures, watchdogs, evictions or reservation stalls reported. Coverage
had351 unready/source-fallback assets during initial admission, then zero for
subsequent sampled windows. Fixed initial view reached zero refinement fallback
and zero requests. This is not proof of cached sub-1s acceptance or hole-free
motion at every frame. Goal remains active.

Native async_stage_pipeline_tests rebuilt and ALL PASS: overlap, bounded
admission, cancellation, failure propagation and shutdown resource pins. This
checks the queue contract; editor movement exercises the runtime integration.
