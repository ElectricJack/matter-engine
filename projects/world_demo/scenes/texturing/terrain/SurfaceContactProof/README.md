# SurfaceContactProof

**Functional proof:** the shared world recipe reaches terrain and explicitly
selected placed objects through `streaming.surfaceReceivers: ['ContactReceiver']`.
Native CPU/GPU checks and demand/eager captures demonstrate contact deposition
and POM. Final material art, chart/seam and motion acceptance remain open.

Open `SurfaceContactProof` in the editor. This bounded fixture tests one
world-space dirt, dampness and moss recipe across actual terrain, an analytic
rock and a simple foundation wall. The foundation is a twelve-triangle box;
its blocks and deposits are composed in VT with physical height for POM.

The objects are installed and placed by `WorldSector`; streaming worlds do not
place ordinary `static roots`. The shared JS recipe is `shared-lib/surface_contact.js`. The original triangle
material selects the substrate and controls contact coverage:

| Receiver | Original ID | Contact layers |
| --- | ---: | --- |
| Terrain | 16 | Allowed inside volume |
| Rock | 11 | Allowed inside volume |
| Wall front | 9 | Allowed inside volume |
| Wall back/cap and elevated shelf | 17 | Excluded everywhere |

The second wall behind the foundation has an allowed front ID but lies outside
projection depth. The protected shelf overlaps the volume to test categorical
exclusion separately from spatial bounds. Each object has a distinct variant;
the adapter accepts one rigid placement per variant. Unsupported/shared
placements keep asset shading, and selected finite-source modules fail explicitly.
This fixture does not solve world overlays on repeated instances or existing
finite geometry-baked brick modules.

No atlas, texture geometry bake or physics settling is requested. Rock geometry
uses deterministic analytic placement. Terrain density is 128 texels/m; use
`MATTER_VT_PROP_TEXELS_PER_METER=128` to match the recorded prop review density.
This is a contact/POM mechanism proof, not final art or a new production density
policy for StreamMountain.

[Native tests, captures and current limits](../../../../../../docs/agent/evidence/2026-09-17-surface-contact/README.md).
