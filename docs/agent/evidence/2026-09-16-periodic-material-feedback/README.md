# Receiver and material feedback

Status: paired visible-demand transport implemented; all 12 native checks pass.
This is a prerequisite for the pending retained wall mapping/POM integration.
Existing wall draws still emit their receiver request and an empty module request.

## Transport

The existing depth-tested G-buffer attachment is now `RGBA32_UINT`. Each
component carries the receiver's existing u16 field in its low half and the
independent module's u16 field in its high half. The low half of W retains the
captured input-snapshot and composed-height tags used by primary ray tracing.
Updating those tags preserves the module mip in the high half.

`vt_material_visible_feedback` packs the two addresses resolved by the material
sampler. GPU extraction reads the same fixed sample in every 8x8 block, including
partial right/bottom blocks, and writes two contiguous four-u16 request planes.
The existing CPU collector merges and deduplicates both planes before queueing
pages. A zero owner ignores all other fields. Neither pixels nor frames alternate
between receiver and module demand. Both requests share final fragment depth
visibility; the number of G-buffer color attachments remains seven.

The raster attachment, raster pipeline format, RT storage-image declaration,
dummy image, extraction shader and cached readback ring use the new format.
An incompatible attachment is rejected before replacing the active readback view.
GPU sampler probes also verify that primary-input tags cannot overwrite module
mip demand.

## Memory cost

The full-resolution attachment increases from 8 to 16 bytes per pixel. The three
cached readback buffers increase by 8 bytes per sampled 8x8 block each. At
1920x1080 this adds 15.82 MiB of logical image storage and 0.74 MiB of readback
storage, before driver allocation alignment. This is an explicit integration
cost, not a claimed frame-time or overall-memory improvement. Existing configured
VT pool reservation is unchanged. Scene timing and overall memory acceptance
remain open.

## Native coverage

The new `vt-feedback-pair` mode registers a finite receiver and an independently
owned periodic module with the real compositor. The shipped material sampler
produces distinct owner/page/mip requests. A fixture uploads those GPU-produced
words to an odd-sized visible image; the production extraction shader, cached
readback, CPU deduplication and residency queue must generate exactly the two
requested detail pages. Duplicate demand, module-only and receiver-only samples,
tagged background, unsampled pixels, format rejection and resize are checked.

This fixture does not claim to draw a module-mapped wall: its visible image is
provided explicitly. The existing `vt-feedback` renderer check separately covers
overlapping charted/uncharted geometry in both draw orders, occlusion, reveal,
return and odd-size target changes. Raster/RT input and POM regressions must also
pass with the wider attachment.

All five MSVC targets build, including the editor. The paired transport,
production visibility, independent sampling, module ownership, input snapshots,
ordinary/connected POM, direct sources, chart atlas, CPU residency and compositor
checks pass. GPU checks report zero Vulkan validation errors with no RT skips.
The [final audit](final-audit.json) verifies 922 frozen source hashes, all five
binaries and all twelve test logs. These checks do not establish scene timing.

Evidence prefix: `feedback-pair-v1` in the sibling
`2026-09-16-shared-vt-pixels` directory. Use `build_checks.py` with a fresh prefix
and `run_checks.py PREFIX vt-feedback-pair vt-feedback vt-material-domain
vt-module-residency vt-input-snapshot vt-composed-parallax vt-composed-seam` to
repeat the focused checks against a frozen native build.

Remaining integration: retain and publish chart mappings with module leases;
resolve shared pixels and metre-valued height from physical wall positions
through ordinary and connected POM; emit the mapped module request from actual
wall draws, including displaced sample coordinates. Sparse weathering and the
broader terrain/contact/acceptance work remain open in the
[material-domain plan](../../../superpowers/plans/2026-09-16-periodic-material-domains.md).
