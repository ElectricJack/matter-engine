# Primary RT input compatibility: historical draft review

**Superseded status:** the rebased production change and regressions are now applied and tested; ordinary input waits are also removed. See [current evidence](README.md). The draft-time findings and artifact descriptions below are preserved as history.

Status: **draft only**. No production sources or evaluated binaries changed.
The StreamMountain editor (PID 46124) remained live and responsive during this
work. Both patches pass `git apply --check`; neither has been compiled or run.
[Checks and artifact hashes](primary-input-draft-checks.json) preserve that
distinction. The full V0–V7 goal remains active.

## Current-source findings

The tested G-buffer chooses its material from the resolved page snapshot before
detail/POM. Secondary RT surface lookup also has snapshot selection. However,
primary lighting still reads the live material table for proxy reconstruction,
the POM roof escape, transmission and reflection. The shadow raygen separately
reads the live table for proxy reconstruction, receiver classification and the
roof escape. Consequently, holding page replacements does not hold every
primary shading input. This is a source-derived mismatch, not yet a new native
red/green result.

`gbuffer.frag` already writes an RGBA16_UINT visibility attachment. Its fourth
component currently carries only a requested mip; `VT_MAX_MIPS` is eight.
Finished surfaces produce no demand, although the G-buffer can select their
captured material. `vt_feedback.comp` currently copies all 16 bits of that
component into compact CPU feedback. A shading tag therefore needs explicit
removal before readback.

The attachment already has storage-image usage and ordinary renderer-target
ownership. Normal frame submission and standalone raster submission retain its
allocation token. Resize replaces it with the other targets. Feedback extraction
runs immediately after `vkCmdEndRendering`, before primary RT dispatch. Its
existing barrier makes color writes visible to compute reads; that destination
scope does not itself name RT reads. The proposed additional transition uses
the renderer's existing `transition_for_use` helper before RT dispatch. That
helper emits a barrier even if the layout is already GENERAL and covers prior
commands, including extraction. The next raster pass already orders earlier
reads before attachment writes. This ordering is consistent with the
[Vulkan memory-dependency rules](https://docs.vulkan.org/spec/latest/chapters/synchronization.html#synchronization-dependencies);
native synchronization validation remains required.

## Prepared production patch

[Production patch](draft-primary-input-production.patch),
[base/draft fingerprints](draft-primary-input-production-source.json):

- Encode `snapshot + 1` in visibility component `w`, bits 4–7; retain mip in
  bits 0–3. Zero means live/fallback inputs. Finished surfaces carry a tag even
  when their request layer is zero. Chartless and impostor fragments carry no
  version tag.
- Strip the tag during compact feedback extraction and preserve an all-zero
  no-demand record. CPU feedback layout and page addressing stay unchanged.
- Add one read-only storage-image descriptor at RT set 0, binding 29, pointing
  to the existing attachment. Increase RT storage-image pool capacity by one
  descriptor per frame. The patch allocates no additional image.
- Resolve primary material rows through the existing captured material banks.
  Lighting uses its mapped G-buffer source pixel, including scaled GI; shadow
  and neighbor-proxy helpers use their corresponding full-resolution pixels.
  Captured lookup precedes the existing live-table bounds check/fallback.
- Update fixed/adaptive lighting through their shared implementation, plus the
  separate shadow shader. Add the new headers to rollback Make dependencies;
  CMake already uses compiler-generated include dependencies.

There are eight production files in the patch, including two new GLSL headers.
The change leaves ordinary source/input idle waits in place. Descriptor-limit
handling and timing costs still need acceptance; a patch that applies cleanly
does not establish Vulkan correctness or speed.

## Prepared regression patch and execution order

[Regression patch](draft-primary-input-regression.patch),
[base/draft fingerprints](draft-primary-input-regression-source.json) extend
the existing production `vt-input-snapshot` mode:

1. Establish a dielectric reflection control with raw reflection alpha 0.04.
   This alpha is the selected material F0, independent of stochastic ray color
   and denoising. Hold replacements, change specular strength, and require four
   frames to retain 0.04 along with the old G-buffer inputs. After publication,
   require the new 0.12 value. Control failures must be resolved before treating
   a failing held-frame assertion as the intended reproduction.
2. Enable finished-surface POM and establish measurable displacement. Hold
   replacements while changing the source height range. Require the existing
   four-frame albedo/normal/ORM/depth continuity checks, followed by a changed
   depth and displacement after publication. This also exercises a captured
   finished-surface material with zero refinement demand.

When the evaluation session is terminal, apply and build the regression patch
first and record its actual outcome. Then apply the production patch, rebuild,
and run the same fixture with Vulkan validation. Do not assume the predicted
F0 controls or failure count before execution. Keep separate source manifests
for the red and corrected revisions.

Required follow-up includes feedback visibility/reveal/resize, queue/input
pressure, existing POM and VT raster/RT modes, chartless and water paths, and
`cull` frame-resource recovery. Run the registered CTest entry too. Restore the
full native build and rendering matrix before new scene measurements. Ordinary
wait removal, cache/scheduling work and repeated performance acceptance remain
open under the original plan.

One additional acceptance concern is history invalidation: primary RT's scene
key hashes the live material revision, while `gi_temporal.comp` compares the
existing material/instance identity and depth/normal guides. Neither currently
consumes the proposed visibility tag. Determine whether delayed publication of
a material-only change retains incompatible filtered history; the proposed raw
F0 regression alone cannot answer that question. Do not classify a denoised
material transition as verified from raw-buffer continuity.

## Reproduction helpers

The external directory
`D:/tmp/matter-vt/20260915-feedback/primary-input-draft/` contains
`prepare_production.py`, `prepare_regression.py`, their generated overlays and
the patches. The helpers only write external draft files. Run from the repo
root; each transformation asserts its original source anchor. The checked-in
evidence directory contains copies of both patches and fingerprint manifests.
