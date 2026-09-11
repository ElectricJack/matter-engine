# Thin timber native probe

Four roots: a neutral mesh stand, one production `CastlePlank` (1.6 × 0.30 ×
0.10 m), one production `CastleBeam` (1.6 × 0.12 × 0.12 m, mortise and straps),
and an upright beam (0.72 × 0.12 × 0.12 m). Primitive seeds are 3, 2 and 6.
Dimensions are canonical primitive parameters; instance transforms only rotate
and translate. The floor rule spans one metre with centimetre ticks.

The voxel Parts include incised grain, knots and end checks. At detail 1.5,
their finest authored spacing is about 17.3mm and engine bake version 9 samples
them on the capped 15.87mm lattice. Timber author simplification is disabled.
Part LOD budgets contain only `[1]` and impostors are disabled.

After building the updated editor with the integration checkout's PhysX flags,
run from native PowerShell:

```powershell
& .\projects\world_demo\scenes\CastleTimberProbe\capture.ps1
```

The script captures raster/RT overviews, matching plank-groove close-ups, RT
beam joinery and an upright furniture leg. It does not build the editor. Output
goes to `D:\tmp\matter-castle-timber-probe` and is copied to
`build/qa/castle-timber-probe`. Check that the broad tabletop stays flat, grooves
remain shallow, and the beam/leg silhouette remains continuous without fins or
black holes. Successful execution alone does not establish visual acceptance.

Authoring metadata from the real Part-prelude harness is saved separately in
`build/qa/castle-timber-probe/authoring-metadata.json`; native baked triangle
counts are reported by the editor's flatten log after capture.
