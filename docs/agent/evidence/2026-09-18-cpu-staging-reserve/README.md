# CPU raster staging reservation

Renderer initialization accepts CPU vertex/index staging reserve sizes and
pre-touches them while preserving existing logical sizes/content. The audit
profile uses 1024/128 MiB; defaults remain zero outside this opt-in profile.
This prevents vector relocations within that capacity, but is not a hard-cap
bank allocator. Exceeding capacity still grows. Vulkan reservations unchanged.

Editor build passed (/tmp/cpu-staging-reserve-build.log), Python syntax passed.
Strict scene run C:/tmp/matter-blas-mountain/cpu-staging-reserve-v1 passed with
all expected cached identities/assets, no compilation or geometry coverage gaps.
Full readiness 10.224s includes initialization. Small source registrations before
renderer initialization caused 14 growth events (largest <1ms); no growth after
initialization. Largest measured geometry registration 6.028ms, compared with
223.404ms directly measured vertex-vector relocation in raster-growth-v1.
Upload-loop total 310ms, largest 6.615ms. End-to-end loading remains incomplete;
a single run cannot establish a repeatable full-load speedup.

Known remaining issues: shader detail traversal rejects off-frustum nodes, but
SectorStreamer::next_request ranks requests by distance/hole bonus only. Anchor
handoff carries position without camera planes. Visible-first scheduling must
carry a conservative camera snapshot across the coordinator boundary, rank
visible requests before background requests while preserving seam dependencies,
and update on camera rotation. Current audit waits for global readiness and is
not yet the acceptance test for the user's clarified visible-set target.
