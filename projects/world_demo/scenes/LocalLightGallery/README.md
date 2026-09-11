# LocalLightGallery

Raster acceptance fixture for indexed analytic local lights. The main world
contains a 17 x 17 point-light grid (289 points) plus two spots, with authored
sun and sky both black. `LocalLightPointIsolation` and
`LocalLightSpotIsolation` use the same receiver geometry with only the named
light type enabled.

The initial deferred-raster path is deliberately unshadowed. `castsShadow`
remains an RT visibility request; fins in the fixture make that limitation
obvious rather than implying a raster shadow atlas exists.

Use debug-view index 7 (`Local-light candidates`) to inspect the world-space
hashed lookup. Production shading iterates only that cell's compact list plus
the explicit oversized-light fallback, never all 289 records.

Canonical capture from WSL after the MSVC build:

```bash
cd MatterEditor
MATTER_WORLD=LocalLightGallery MATTER_RT=0 \
MATTER_SCREENSHOT='C:/tmp/local-light-gallery-raster.png' \
MATTER_SCREENSHOT_SETTLE=120 ./build/windows-msvc/editor.exe
```
