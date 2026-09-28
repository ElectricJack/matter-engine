# Separate-receiver surface contact proof

Status: receiver categories and explicit world-recipe binding pass native checks.
Capture v4 demonstrates the bounded field on terrain, rock and foundation with
POM. Final material art, seam and full terrain/building acceptance remain open.
The rejected v1–v3 captures below describe the earlier binding gap.

## Mechanism

`s.receiverMaterial` reads the original low-byte triangle material, before a
world direct-source recipe replaces its output carrier. It is categorical and
constant over the receiver triangle, including all height-derivative samples.
This generic input can select base recipes and allow or exclude deposition.
It requires source v1; legacy vertex weights and habitat reject it. Reusable
prepared face sources also reject it because they have no eventual receiver.

The CPU reference takes an explicit receiver material (default zero for old
callers). GPU composition reads the packed triangle material directly. The
shared planar-page content key now includes original receiver identity; mixed
material rectangles use the ordinary page path. No new descriptor or physical
VT format is introduced.

The JS recipe lives in `projects/world_demo/shared-lib/surface_contact.js`.
`SurfaceContactProof`, grouped under `scenes/texturing/terrain`, has an actual
density-field ground, separate analytic rock mesh, twelve-triangle foundation,
protected elevated shelf, and a second wall outside the authored volume.
The authored recipe selects front-wall, rock and ground IDs for common
world-space dampness, dirt and moss. CPU evaluation excludes back/cap/shelf IDs
even inside that volume and excludes the remote wall by depth. The live scene now selects `ContactReceiver` through
`streaming.surfaceReceivers`. Its four uniquely placed rigid variants use their
complete expanded child world transforms, matching the terrain coordinate frame.

Dampness changes appearance. Dirt and moss deposit positive metre thickness
through `s.splat` / `s.layer`; color, squared roughness and height use the same
weights, with normals derived from the final height POM reads. No source atlas,
geometry texture bake or settling job is requested. This is a bounded
functional fixture, not final material art.

## CPU evidence

Native MSVC prefix `receiver-material-input-v1` in the sibling
`2026-09-16-shared-vt-pixels` folder builds and checks actual native parsers,
JS evaluation, GPU packing and source sampling:

- `surface_field_tests`: category-specific source/appearance and constant
  carrier; reject receiver input in legacy classification and habitat.
- `face_material_bake_tests`: reject receiver-dependent reusable source recipes.
- `vt_residency_tests`: different original categories cannot alias shared pixels;
  mixed categories fail the uniform rectangle optimization.
- `world_definition_tests --surface-contact`: actual imported recipe fits in
  364 GPU ops. Sampled height [-0.033019, -0.007957] m; maximum sampled contact
  deposit 0.006523 m. Protected categories and outside-volume probes preserve
  all channels exactly. Translated receiver frames evaluate identical fields.

## Scope still open

World anchoring still requires a variant referenced by one instance. This proof
uses distinct variants; it does not solve repeated-instance world overlays.
The input is a material category, not a stable instance ID or general receiver
tag index. Facing protection here is authored per-face category; automatic
facing/contact queries are not implemented. Normal/field-derived lanes remain
unsupported in source height until their derivatives exist.

The world recipe does not yet overlay geometry-baked finite/periodic brick
bases: those bind their own local recipe. Sparse splat records, spatial culling,
local bounds invalidation and per-instance sparse overrides remain open. All
three splats here compile into one bounded recipe and evaluate on its pages.
Do not extrapolate the small proof's generation time to StreamMountain startup.

## Native GPU checks and initial scene diagnosis (v1–v3)

All nine `receiver-material-input-v1` checks pass: surface, face-material,
residency, contact, full compositor, composed POM, composed seams, periodic
receiver mapping and module residency. The compositor reads actual packed
triangle IDs 1, 2 and 255 and verifies the resulting BC7 color and metre height.
Vulkan validation reports zero errors. Seam fixtures retain displaced depth on
straight, curved, diagonal and 90-degree transitions.

`receiver-material-input-v2` moves the scene's review density into the correct
`streaming` block and makes rock caps planar. Contact and StreamMountain native
checks pass. `receiver-material-input-v3` corrects receiver placement through
`WorldSector.requires()` / `placeChild()` and shifts the almost-flat ground off
the y=0 cube boundary. Its focused native check verifies four installed variants
and passes. Engine/shader implementation and editor binary remain identical
across v1/v2/v3; only fixture data, its CPU test and documentation change.

Capture v1 completes its transport audit, but is **visually rejected**: world-kind
loading skips ordinary root composition, so all four objects were absent. The
short v2 diagnostic confirms an unresolved root and is deliberately stopped;
its audit fails incomplete views. Capture v3 renders the objects correctly,
removes the visible near-ground holes and completes 24 image/sidecar pairs,
including POM-off and native-RT controls. It has no command or Vulkan errors,
restores exact props, keeps binary/sources immutable, and closes its editor.
It is also **visually rejected as a contact demonstration**: props keep their
original materials. Only ground receives the authored contact recipe.

The missing binding in the v3 implementation was explicit in `WorldSession::Impl::service_vt_rung_requests`:
world classification is guarded by the resident-sector reverse index. Its
instance-count/frame map also counts manifest entries, while drawn child
receivers are expanded nodes with their own transforms. Applying the tape to
all props would overwrite existing foliage/asset materials and is not a valid
fix. That diagnosis required explicit receiver selection, expanded-node
world frames, correct variant-sharing rules, and agreement between demand,
eager registration, live reclassification and fallback. Moving receivers or
changing a recipe must invalidate the affected pages. Existing finite/periodic
bases must retain their separate local source and gain an overlay path.

Ground POM-on/off v3 close and grazing images change >1 byte on about 30.0% and
33.9% of pixels respectively. This is an image difference, not a millimetre
accuracy oracle. The native GPU height and POM tests provide that semantic check.
The props are untextured controls in these images, so this does not accept
building POM or cross-object continuity.

Small-fixture warm root setup is 302 ms and 28 streamed sectors fill in 1.12 s.
Single G-buffer samples are 5.550 / 2.973 / 2.421 ms for overview/close/grazing.
There are zero queued fills, rejections or evictions at these samples. These
numbers are development observations only: the frozen r2 asset editor remains
open, and the fixture cannot establish StreamMountain performance or cold-load
acceptance. StreamMountain's retained recipe remains unchanged.

`receiver-material-input-v4` binds the final documentation-only WIP notice to
the same native editor binary; the build is a no-op. No implementation changed
after the v3 contact check and capture. The capture editor is closed; only the
independent frozen r2 editor (PID 9056) remains open.

## Explicit receiver binding (v4 onward)

[Implementation, evidence and limits](../2026-09-17-world-receiver-binding/README.md).
The default-loading v4 capture completes all 24 images. Substrate colors and
shared deposition appear on terrain, rock and wall; protected back/cap/shelf and
the remote wall remain clear. Close and grazing POM-on/off image differences
exceed one byte on 54.0% and 56.2% of pixels respectively. Native height/depth
checks provide the semantic oracle; image differences alone do not. Lit shots
retain a strong sky tint, rock facets and some visible chart-edge artifacts.
This accepts the limited binding demonstration, not final art/seam approval.

The eager-loading v5 launch is **rejected**: eager source snapshots preceded
fallback-image initialization, producing null descriptors and a startup crash.
The corrected audit records ten Vulkan validation errors. Initialization order
is fixed in native build `world-receiver-binding-v2`; eager repeat v6 passes
all 24 captures with zero validation/command errors. Demand/eager raw channels
agree within one byte except one six-byte albedo pixel. The post-correction
StreamMountain nineteen-image v3 review also passes its audit; its remaining art
and performance limits are recorded in the binding evidence. The fix does not reduce density,
height range or POM quality.
