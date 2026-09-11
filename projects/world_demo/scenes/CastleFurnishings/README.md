# CastleFurnishings fixture

Native fixture for `shared-lib/castle_furnishings.js`: a furnished hall,
chamber and chapel corner. Run it as world `CastleFurnishings`; capture it with
`capture.ps1`, which saves raster and native-RT shots to
`build/qa/castle-furnishings/` and fails on bake, flatten or validation
errors.

It contains every furniture family: a trestle table with wedged through-tenons,
butterfly keys, pegs and gold bosses; benches; a chair and a gilded throne
with velvet cushions; a canopied bed; an iron-bound chest; a strap-hinged
cupboard (placed with the `cabinet` alias); two coopered barrels; and a stone
altar with linen, a gold cross and candlesticks. It also has open and glazed
lantern sconces (one with a spot), an eight-candle chandelier, and three
glazing units: stained true-thickness glass with a pointed head, a clear
two-light window with a quatrefoil oculus, and a rectangular pane flagged as
thin glass.

All furnishings come from one `furnishingPlacements(records, { materials })`
call. `layout.roots` feeds `World.roots` and `layout.lights` feeds
`World.lights.points/spots`. Each lit fixture therefore has an RT-visible
body, a `rayTraced(false)` flame glow, and analytic lights, all from one
placement transform. The backdrop walls (`objects/CastleFurnishingsRoom.js`)
cut the same window openings using `glazingSpringY`/`glazingHalfWidthAt`.

The Vulkan renderer only shades analytic local lights once the lighting
tasks land. Until then the fixture shows the emissive flame proxies and the
dusk sun/sky; the light records are still published through the provider.
