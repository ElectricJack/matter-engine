# Explicit world recipes on placed receivers

Status: native CPU/GPU checks plus default and eager contact captures pass. Eager
loading exposed an initialization crash; the corrected build completes the proof.
This is a functional checkpoint, not completed terrain/building visual acceptance.

## Authoring and binding

`World.streaming.surfaceReceivers` is an optional array of up to 64 unique,
nonempty module names. Selected modules must be installed by
`WorldSector.requires()`, and the world must provide a direct source. Ordinary
unselected assets retain their existing materials. Selected finite-surface
sources fail explicitly: world overlays on those local bases are still separate
work. The contact fixture opts in only `ContactReceiver`.

Binding resolves the physical expansions of every resident manifest root,
including nodes hidden by draw filters. Parent and relative child transforms
compose as full matrices. One rigid, static placement can use its world frame;
multiple placements, scaled/sheared/reflected frames and unsupported animation
or shared-surface representations retain asset shading. This is not the planned
per-instance overlay system. It avoids silently treating origin coordinates or
one visible instance as the frame for a shared variant.

The receiver table updates when world placements, the source recipe or renderer
expansion changes; worlds with no selected receivers take the empty fast path.
Registered pages receive copied weights, field lanes and a row-major 3x4 world
frame in one immutable source update. Demand registration consumes the same
frame table. Eager registration is updated after instance expansion. Only
selected affected variants participate; source updates retain prior completed
pages while replacements are prepared under the existing VT update contract.
This does not yet prove motion/edit-latency acceptance or sparse region updates.

## Related vertical-sector correction

Demand registration's terrain reverse index previously held X/Z translation
only; live sector classification also omitted Y. Both now include the volumetric
sector Y offset. Initial worker classification already included it. World-space
altitude and 3D noise therefore use the same physical cube placement in these
paths. The shader, density policy and material recipe are unchanged, but terrain
appearance at nonzero vertical sector coordinates can change to the correct
world-space values. The subsequent StreamMountain capture below reviews this correction.

## Current checks

`world-receiver-binding-v1` in `2026-09-16-shared-vt-pixels` records the immutable
source set and native MSVC builds. Focused contact, CPU residency and mountain
material checks pass. CPU receiver checks cover rotation, child translation on
all axes, two physical placements, scale/shear rejection and nonfinite frames.
World-load checks reject malformed/duplicate/excessive module selections and
retain the proof's explicit selection.

## Captures and startup regression

[Contact capture v4](../2026-09-17-surface-contact/v4/audit.json) completes all six
views and 24 PNG/sidecar pairs with immutable sources/binary, restored props and
no Vulkan validation or command errors. The wall, rock and ground now share the
bounded damp/dirt/moss field. Raw albedo confirms their authored substrate colors;
the strongly blue lit result is not their base-color output. The back, cap,
elevated shelf and remote wall remain visibly free of deposition. POM-on/off
changes >1 byte in at least one channel on 54.0% / 56.2% of close/grazing pixels.
This difference is not a depth oracle; native composed-height/POM checks supply
that semantic test. Native RT also renders the selected receiver layers, with
remaining raster/RT lighting differences and visible chart/facet artifacts.
The proof is not approved material art or complete seam acceptance.

Default-loading native builds and nine focused checks are recorded by
`world-receiver-binding-v1`: contact, residency CPU, mountain material, VT queue,
immutable input snapshots, composed parallax, composed seams, periodic receiver
mapping and module residency. All pass; GPU modes report zero validation errors.

Capture v5 (`--eager`) fails before its first view: immutable draw-input bindings
captured null fallback images because eager part publication preceded renderer
initialization. The failure remains recorded. `ensure_vt_runtime` now initializes
source fallback images before creating its first immutable input snapshot.
Capture audits also recognize both `Validation Error` and `validation ERROR`
log formats; the v5 audit was corrected to record its ten validation messages.
`world-receiver-binding-v2` builds the fix. Native contact, mountain, input
snapshot, composed-POM and composed-seam checks all pass on its frozen sources;
GPU checks report zero validation errors.

[Eager repeat v6](../2026-09-17-surface-contact/v6/audit.json) exits zero with
all 24 images, no validation/command errors and restored props. The editor SHA
is `c503c7d16294eddc8048f1628a692d745bb09b1cd86d5a91f5491bcfe60479c5`.
[Demand/eager pixel comparison](../2026-09-17-surface-contact/v4-v6-image-comparison.json)
finds all raw normals and all but one raw-albedo pixel within one byte. The
single back-view albedo outlier is six bytes. Lit raster differences occupy
at most 0.0206% of pixels above one byte; RT differs above one byte on 1.41%.
Both RT views visibly contain the same substrate/deposition structure, but this
is not exact lighting parity acceptance. Eager retains 32–38 variants versus
11 for demand in this fixture; it is a diagnostic route, not the default.

## StreamMountain follow-up

[Capture v3](../2026-09-17-mountain-layering/v3/audit.json) completes all nineteen
images with zero validation/command errors, immutable source/binary identity,
exact props restoration and its editor closed. Only the independent frozen r2
editor (PID 9056) remains open. The production mountain recipe, density and POM
settings are unchanged. Close/grazing controls still show embedded stone relief;
cliff controls retain relief too. Source evaluation can change intentionally at
nonzero sector Y; this is not a pixel-identical material update.

Overview/grazing/frontal-cliff/oblique-cliff/close G-buffer medians are
32.9625 / 16.837 / 27.866 / 36.7255 / 5.5275 ms (30 samples each).
Root setup is 54.619 s, including 42.275 s publish; 2,586 sectors fill in
168.37 s. These are non-isolated observations, not speedup/acceptance evidence.
There is no clear additional timing regression in these views, but the earlier
performance issue remains unresolved. Fine pale stones remain busy, moss/soil
need detail, cliff forms remain plate-like and chart/LOD seams are still visible.
The full goal and visual approval remain open.

The proof scene README was updated after captures. `final-source-audit.json`
records that documentation-only delta; all compiled/authored runtime inputs and
the executable still match the tested v2 source manifest.

## Remaining scope

Per-instance sparse overlays, contributors stored independently of the material
recipe, local splat-bounds invalidation, automatic contact/facing queries and
world layers over finite/periodic geometry-baked wall bases remain open.
POM stays the displacement solution; voxel silhouettes remain deferred.
