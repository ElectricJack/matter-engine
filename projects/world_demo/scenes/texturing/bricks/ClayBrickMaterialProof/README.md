# Textured clay source inspection

One actual `clayBrickSourceSpec` solid with firing-color variation, roughness,
fine material pores and grain from `clayBrickMaterial`. The source also has
rounded/chipped edges, deeper dents, 60 pockmarks and six scratches per broad
face. No physics is used.

The scene uses a streamed receiver because the current world direct-source
tape attaches there. Its GPU geometry bake uses the same queued service as
authored roots. This is a high-resolution source inspection mesh, not the
finished low-poly wall or prepared finite-stamp integration.

Launch with `MATTER_WORLD=ClayBrickMaterialProof`. Close inspection camera:
`cam 0.095 0.115 0.34 0 0.043 0.025`. Recorded review images use 2048 prop
texels/metre and explicit lighting/exposure settings in the evidence manifest.

See [geometry and material evidence](../../../../../../docs/agent/evidence/2026-09-15-clay-brick/README.md)
and `ClayBrickGeometryProof` for the eight variants in flat diagnostic colors.
