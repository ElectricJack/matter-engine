# ProceduralBrickProof

Direct-field brick / peeling-paint proof. Open world `ProceduralBrickProof`.
The nearest origin sector emits one wall centred at (4, 1.25, 4). Its material
is evaluated into VT from physical coordinates, without a Wang source image or
physics texture bake. `s.cellNoise2` varies individual bricks and `s.layer`
composes paint color, roughness and thickness over the brick/mortar substrate.

Use `MATTER_VT_PROP_TEXELS_PER_METER=512` for the current shape review; the
16 texels/metre default provides only about four texels across each brick.
Captured exposure is -0.5 EV. The capture harness explicitly pins its review
lighting and POM controls; exact settings are in the capture manifests.

[Native close/middle/far/grazing/RT views and evidence](../../../../../../docs/agent/evidence/2026-09-15-material-look/README.md).
The look remains a prototype. Composed-height POM is connected in raster and RT;
general splat placement, boundary/filter acceptance and performance remain open.

Current weathering recipes and native captures: [bounded splat checkpoint](../../../../../../docs/agent/evidence/2026-09-15-weathering/README.md).

Latest [shape review and matched comparison](../../../../../../docs/agent/evidence/2026-09-15-material-shapes/README.md):
255 x 95 mm running-bond pitch, small laying/firing variations, rounded chipped
edges, independent 6–9 mm bevel width, 4 mm mortar recess, quieter face relief,
and 0.18–0.48 mm paint with sharp footprint-aware coverage. The base height range
is [-8, +6] mm before paint/moss deposition, including tilt and grain extrema.
