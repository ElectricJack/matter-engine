# ProceduralTerrainProof

Native density-field terrain with direct rock / soil / moss material generation.
No scatter catalog, periodic source image or physics texture bake is requested.
World-space material signals cross sector boundaries; `s.layer` composes soil
replacement and moss deposition through the same helper used by the brick proof.

Use the existing `MATTER_VT_PROP_TEXELS_PER_METER=256` override for review of this
small fixture. The captured exposure is -1 EV; camera/lighting/seeds and physical
height ranges are in the scene. The native mesher uses 2 m cells, so the geometry
is intentionally coarse. This is not final art or a large-terrain density policy.
The saved terrain streaming radius is 24 m, matching the world declaration;
expanding it changes this bounded material fixture into a large streaming test.

[Native views, commands and current limits](../../../../../../docs/agent/evidence/2026-09-15-material-look/README.md).

Current weathering recipes and native captures: [bounded splat checkpoint](../../../../../../docs/agent/evidence/2026-09-15-weathering/README.md).
