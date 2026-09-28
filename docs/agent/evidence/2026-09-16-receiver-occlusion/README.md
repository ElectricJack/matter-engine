# Receiver occlusion over shared material

Status: eleven focused native checks pass. This is a checkpoint toward
shared wall bases and sparse layers, not completion of visual/performance gates.

## Problem and change

The previous coverage-only optimization was disabled at the normal RT editor's
two-page/frame AO budget. In-place ORM enrichment also modified private receiver
pixels that mapped shading bypassed in favor of the shared module.

Direct-surface receivers now store the traced part-local AO multiplier separately
from the base material. The existing hemisphere trace writes packed R16 factors
into immutable page ranges. Raster, RT and POM sampling multiply only ORM.R at
the receiver coordinate, leaving shared color, normals, roughness, metalness and
height unchanged. Coverage-only interiors remain eligible with the native AO
producer enabled. Legacy pages/custom producers retain their in-place path.

This remains **part-local self-occlusion**, shared by placements of the same
geometry variant. World-space contact blending and independently authored
instance weathering require subsequent layer work. POM geometry and its sample
quality are unchanged.

## Ownership and memory

- Each 136×136 page including gutters uses 36,992 bytes of R16 factors.
- Device buffers allocate 32 pages per slab (1,183,744 payload bytes). Reported
  allocation bytes use the driver's allocation size, including unused slab space.
- Publication requires a successful producer receipt and current page owner,
  slot generation, content revision, input snapshot and non-dirty state.
- Failed or stale work may already have recorded GPU writes. Its allocation,
  like replaced factors, remains retained through the eight-frame reader horizon.
- Empty slabs are released after their last live/retired page lease expires.
- Editor `STATSVT` reports coverage-only pages, published AO pages, retained AO
  pages, allocated AO bytes and cumulative deferred enrichment.
- Receiver metadata grows by 16 bytes per reserved pool slot (64 to 80 bytes).

The reserved material pool is unchanged. Fewer occupied material pages and
avoided encoding are **not** proof of lower allocated VRAM; this path adds an
explicitly measured AO allocation. No isolated performance result is claimed.

## Validation plan and retained evidence

Native manifests/logs use `receiver-ao-*` prefixes in
[shared VT evidence](../2026-09-16-shared-vt-pixels/).

The integration fixture uses two receivers sharing the actual compressed
material producer. One has a nearby plate in its own traced geometry. It checks
the normal two-page/frame budget, non-AO channel/POM equality, independent traced
occlusion, declined post-write publication, stale generation, deterministic
regeneration, material-phase edits and delayed allocation release.

A grazing-angle POM regression crosses a complete finite boundary page into a
coverage-only interior. Geometry compatibility now compares geometric inputs,
not per-page storage flags, so the crossing does not falsely reject the surface.

`receiver-ao-v1`: CPU checks passed; the native smoke build caught the metadata
readback fixture's old 64-byte record size. Its explicit total is corrected for
the new 80-byte record. The failed log remains preserved.

`receiver-ao-v2`: all four MSVC targets built against 996 frozen source inputs.
Eleven checks pass: CPU residency, receiver material/coverage/occlusion, legacy
enrichment, material domain sampling, queue, input snapshot lifetime, direct
source raster/RT, surface parallax, connected composed seams, offline VT export
and compositor. GPU runs report zero validation errors with no RT skips. Source,
binary and log hashes were rechecked in [the native audit](native-audit.json).

The real AO fixture measured 0.800000 on the clear receiver and 0.119997 on the
occluded receiver, with identical non-AO channels and POM. Eight post-write
refusals stayed unpublished. One slab allocated 1,183,744 bytes before receiver
release; all sparse allocation bytes returned to zero after reader retirement.
These are functional/memory-lifetime checks, not frame-time acceptance.

## Real wall captures

`walls-v3` captures `PeriodicBrickWallProof` at overview, close and grazing views
in raster and native-RT modes. All six 1280×800 PNG/completion pairs landed, the
editor exited normally, and the log contains no Vulkan validation error. All
996 source hashes and all four binary hashes still match the native build.
The three walls render 36 triangles total and retain two shared modules for
their opposite faces. [Capture audit](wall-capture-audit.json).

The run explicitly sets the normal enrichment values: **32 rays/texel and two
pages/frame**, plus the authored preview density of 512 texels/metre. It ends
with 174 occupied page slots, 101 material allocations, **73 coverage-only pages**
and 146 published AO factors. AO slabs reserve **5,918,720 bytes (5.64 MiB)**.
The configured 4,064 MiB pool is unchanged. The 25,600-slot metadata table also
grows by 409,600 bytes; this is not a claim of reduced total allocated VRAM.
No deferred AO work is recorded at the capture checkpoints.

The views show continuous brick courses through the sampled straight-wall page
boundaries. The repeated base remains visibly periodic: unique weathering and
broader variation are subsequent work. User visual approval, moving-camera
acceptance, mapped corners/caps/curves and isolated timing remain open.

![Shared brick wall in native RT mode](walls-v3/native_rt-close.png)

[Overview](walls-v3/native_rt-overview.png) ·
[Grazing angle](walls-v3/native_rt-grazing.png) ·
[Raster close-up](walls-v3/raster-close.png)

Retained failed harness run `walls-v2` inherited a saved zero-page enrichment
budget and used screenshot paths containing spaces, which the legacy FIFO
parser truncated. It is not accepted visual/AO evidence. `walls-v3` uses explicit
AO values and a space-free capture directory, then copies artifacts here. The
misdirected final v2 PNG is retained in that run's directory.

Sparse weathering, terrain/contact blending and the original full
correctness/performance/visual acceptance remain open.
The frozen export handoff is independent of this development build.
