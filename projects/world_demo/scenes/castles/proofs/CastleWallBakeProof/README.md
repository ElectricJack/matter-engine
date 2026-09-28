# CastleWallBakeProof

First end-to-end source-field to textured structural-wall proof. Two rigidly
placed instances share one physical 4 × 3 m wall mesh; the second is rotated 30°.
Fifteen physical 2 m floor flags provide scale. There are no runtime brick
instances, source-brick meshes, or terrain dependencies.

`CastleBrickBondDetail` declares the eight shared source recipes. The provider
evaluates them transiently and projects their front/back faces, then composes
one periodic running-bond tile into the existing `.gtex` transport. Only the
finished atlas is persisted. The first atlas uses 16 identical, genuinely
periodic Wang layers to reuse the existing uploader and compositor; richer
Wang variation and face-specific curve/reveal provenance remain follow-ups.

Open a visible native editor with `MATTER_WORLD=CastleWallBakeProof`,
`MATTER_HIDE_WINDOW=0`, `MATTER_VT_PROP_TEXELS_PER_METER=512`, and an explicit
SSD cache such as `MATTER_CACHE_ROOT=C:/tmp/castle-wall-bake-cache`.
The density setting is a proof override; chart packing may reduce it.

This first milestone bakes unlit color, normals, roughness and height. Existing
chart-VT rendering consumes surface normals/material channels. The material now
authors `detailMode:'surface'` to select part-local height parallax and exclude
legacy ground overlays. This does not displace silhouettes; the measurements
below describe the earlier capture before the parallax integration.
Native visible raster and ray-tracing captures passed on 2026-09-11. The wall
is 12 triangles; both instances share its mesh. Eight source recipes produce
16 face patches, with zero source meshes or source bundles. CPU atlas/detail
checks and the visible GPU projection oracle passed.

Fresh-cache bake pipeline: 2,193 ms, including 4.791 ms GPU projection,
186.222 ms atlas composition, 545.292 ms save/compression and 409.983 ms
load/upload. Repeat cached pipeline: 1,079 ms, including 1,040.983 ms
load/upload and no projection/composition/save. Process-to-final-publication
was 3,649 ms fresh and 1,675 ms cached. These are diagnostic runs with
validation enabled on the SSD cache, not the subsecond target.

Captures and logs: `C:/tmp/castle-wall-bake-proof/` (fresh) and its `warm/`
subfolder. The material remains a first functional proof: regular masonry,
repeating source pitting and flat silhouettes need further art/height work.

Earlier page-only captures disabled the global near-detail overlay with
`render.vt.near_band_m=0` and `near_fade_m=0.1`. That isolated the old
coordinate mismatch before surface materials had their own rendering path.
Those historical captures remain in `C:/tmp/castle-wall-bake-proof/page-only/`
and `D:/tmp/Castle Screenshots/2026-09-11 Textured Wall Proof/`.
The current `detailMode: 'surface'` material bypasses this overlay itself.

A visible recovery run also passed after deliberately truncating this proof's
cached atlas to its matching 48-byte header. The provider reported one
rebuild, regenerated the final atlas and rendered all four views. Healthy
loads do not perform a preliminary full decode. Recovery receipts are in
`C:/tmp/castle-wall-bake-proof/recovery/`.

## Connected parallax

The scene now opts into `detailMode: 'surface'`. Height drives bounded local
parallax and all material channels follow the displaced coordinate. Toggle
`set render.pom.enabled false` / `true` for a matched comparison; the default
is enabled. No global near-band override is needed for this material.

Visible raster and RT depth/registration tests passed, including rotated
walls and secondary hits. The RT seam correction carries parallax distance
in the half-float ORM alpha channel to recover the original proxy ray origin.
The outer silhouette and collision remain the 12-triangle shell.

Latest screenshots and comparison slider:
`D:/tmp/Castle Screenshots/2026-09-11 Wall Parallax/comparison.html`.
See `docs/designs/castle-surface-parallax.md` for the contract and checks.
