# Geometry-derived surface parallax

A material with `detailMode: 'surface'` uses its finished detail atlas in part-local coordinates. `CastleWallBakeProof` opts in. Existing ground materials retain their prior behavior. The material flag occupies bit 5 of surfaceFlags / GPU flags_misc.x; material schema version is 6.

The raster vertex stage carries local position at location 21, alongside the existing local normal and inverse-transpose matrix. The fragment stage transforms the view ray into local metres per world metre without normalizing it. The baked height maximum is the shell datum; normalized height decodes to nonnegative inward depth. A bounded linear march and binary refinement locate the intersection, then color, normal and ORM use that same local coordinate. Reversed-Z depth can only move away from the camera.

The finished-material branch bypasses both legacy ground POM and the live Wang overlay. It samples the existing GPU detail textures directly rather than having an unshifted chart page overwrite the parallax result. Until the detail slot is valid and loaded, the material retains its base/VT fallback. The legacy triplanar compositor convention is preserved, including mirrored back-face content. This first implementation does not provide unique face UV provenance.

`render.pom.enabled`, steps/refinement, distance/fade, relief cap and maximum travel control this path. Ground datum bias is deliberately excluded: the atlas supplies physical height. Zero steps disable only displacement, retaining normal/material detail. Near-parallel or out-of-budget rays fall back to the shell, and zero-range/maximum-height textures stay on it.

RT closest hits use the same helper and retain the undisplaced proxy position for visibility and outgoing rays. Tagged payload storage holds finished color, ORM and shading normal; a new 12-byte proxy-position field separates shading position from ray origin. Primary G-buffer ORM alpha carries the marched world-ray distance for opted-in surfaces. The existing ORM attachment uses RGBA16F (+4 bytes/pixel, no additional attachment) so this is not limited to 8-bit precision or distances below one metre. RT undoes the displacement to recover the proxy origin; its geometric-normal stencil uses recovered neighboring proxy points with matching material and instance identity. Ordinary ground keeps horizon visibility in ORM alpha and retains its previous roof-escape handling.

This is parallax, not tessellation or an SDF intersection primitive. Outer silhouettes, collision and hardware visibility still use the low-poly shell. Proxy-origin handling avoids self-intersection; it does not create exact brick-by-brick cast shadows. Sharp outer edges still require authored geometry.

Validation target: `castle_surface_parallax_checks`. Visible native mode: `MATTER_VK_SMOKE_MODE=surface-parallax`, `MATTER_HIDE_WINDOW=0`. The gate checks analytic constant recess at yaw 0/30/90, independent color/normal registration from depth, raster and secondary RT agreement, bounded grazing travel, on/off and zero-height behavior. Captures use `C:/tmp/castle-wall-parallax/timeline.txt`. Native material-registry and world-definition suites passed. Visible GPU modes `surface-parallax`, `vt-normal-frame`, `tileset` and `raster` all passed with zero validation errors. The constant 1 cm recess at cosine 0.605733 produced 0.016508–0.016509 m ray travel, matching the analytic 0.016509 m. The ORM transport also recovered the original proxy plane within the native test tolerance at all three rotations.

Accepted visible captures are under `C:/tmp/castle-wall-parallax/final/`; the before/after slider and copied screenshots are in `D:/tmp/Castle Screenshots/2026-09-11 Wall Parallax/comparison.html`. The earlier root-level captures preserve the rejected depth-normal seam version for diagnosis. The final native RT views remove those dark joint seams.

The legacy immediate raster path also needed its local-direct image cleared and transitioned with the other composite inputs; the tileset regression exposed an undefined-layout read. This fix is exercised by the passing tileset/raster modes.

The final guard build and visible `surface-parallax` rerun also passed after
excluding impostor cards from displacement decoding and matching the raster
requirement for a valid charted part. Logs: `C:/tmp/castle-parallax-tests/guard/`.
The full prior four-mode renderer run is in its sibling `final/` directory;
CPU material/world suites passed in `C:/tmp/castle-parallax-tests/`.
