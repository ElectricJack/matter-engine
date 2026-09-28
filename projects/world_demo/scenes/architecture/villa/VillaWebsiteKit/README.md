# Website villa kit

Twenty static opaque parts plus the existing Doric column. Source geometry is in shared-lib/villa_website_kit.js. Their modules live in project `objects/architecture/villa/` because the shared library names them and can be imported by other scenes. Scene-specific detail tilesets remain in this scene's `objects/`. World roots form a spaced inspection grid; asset.export exports each part in its own local coordinates. Launch the frozen r2 build with -World VillaWebsiteKit -TextureDensity 128. The runtime and engine helpers remain fixed.
