# Procedural demo recipes

Import `recipes` or `recipeById` from `index.ts`. Each entry is a
`RecipeDefinition`: `id`, `name`, `description`, `parameters`, and
`build(overrides?, seed?)`. Every parameter declares `type`, `label`, `default`,
`minimum`, `maximum`, `step`, and optional `unit`. Controls are numeric;
`integer` controls use step 1. The SDK rejects unknown keys, nonfinite values,
fractional integers and values outside the declared bounds.

```ts
import { recipeById } from './recipes/index.ts';

const gear = recipeById.get('gear')!;
const document = gear.build({ teeth: 20, depth: 0.4 });
// Send this immutable contract-v1 RecipeDocument to the geometry worker.
```

Seed is an explicit uint64 encoded as sixteen lowercase hexadecimal digits,
default `0000000000000001`. Rock uses it for deterministic silhouette variation.
Other recipes preserve it in the recipe identity and have deterministic geometry.
No ambient randomness, host services, fetched buffers or asset dependencies are
needed. The SDK is imported directly from `packages/matter-authoring/index.mjs`;
Vite can consume these TypeScript modules without additional recipe dependencies.
The application scaffold owns its Vite/TypeScript/three.js versions and UI.

Geometry uses metres, right-handed coordinates and Y up. Outputs retain their
authored origins; the viewer should frame the evaluated asset bounds. Materials
are opaque PBR colors, including the crystal cluster. Multi-part objects publish
multiple closed prototypes or placements rather than joining every touching
component. Chair slats, crystals, chain links and facade windows share mesh
prototypes. Tree forks retain a small capped tip radius (5% of the trunk radius)
to avoid the native cone's zero-radius apex winding defect tracked by
`bold-vault-75`.

| ID | Construction |
| --- | --- |
| `rock` | Seeded faceted closed triangle mesh |
| `tree` | Tapered trunk, branches and forks |
| `pillar` | Extruded fluted contour, plinth and capital |
| `arch` | Box minus rectangular and round doorway cutters |
| `gear` | Toothed polygon with bore, extruded profile |
| `vase` | Revolved hollow wall profile and closed bottom |
| `chair` | Seat, legs, rails and shared slats |
| `crystals` | Shared hexagonal pointed prisms with tilted placements |
| `stairs` | One extruded zigzag profile |
| `chain` | Shared oval torus links with alternating placement rotations |
| `terrain` | Height grid, matching side walls and closed base |
| `facade` | Shared single-window panels, border strips and cornice |
| `bracket` | Unioned right-angle bracket minus four cylindrical bolt holes |

Custom operations declare their actual capabilities through the SDK. CSG stores
0.15 m arch sampling, 0.125 m bracket sampling, zero smoothing and a
0.1 m reference radius in `solid.ordered` nodes. The reference radius shifts the
sampled surface inward by approximately 15 mm for the arch and 7.5 mm for the
bracket, avoiding zero-level grid coincidences across the control ranges. These
are preview dimensions; the serialized settings travel with the document.
The intended preview budget
is 20 million operations, 200,000 vertices and 600,000 indices per recipe,
with the service's default 64 MiB working/output limits. The native test applies
a two-second per-job ceiling and a stricter two-second aggregate check for each
13-recipe catalog, including decode, evaluation, topology checks and placements.
The desktop/browser host can impose its own cancellation and admission policy.

## Native verification

Node 22.18+ (built-in TypeScript stripping), TypeScript 5.7+, CMake 3.25+, Ninja
and a C++17 compiler are required. The gate uses the actual production native
library and the exact SDK-authored documents, passed over stdin in one process.
Mesh data and resolved inputs stay in memory; no geometry caches or mesh files
are produced.

From the repository root:

```sh
cmake -S web/procedural-demo/tests -B /tmp/matter-demo-recipes -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/matter-demo-recipes --target matter_demo_native_recipes --parallel 2
ctest --test-dir /tmp/matter-demo-recipes --output-on-failure
```

`matter_demo_recipes` checks SDK validation and deterministic construction, then
evaluates all default, all-minimum and all-maximum parameter sets. It requires
nonempty validated assets, valid indices, finite bounded positions, conservative
prototype bounds, bounded transformed positions, positive solid volume, and
exactly two oppositely wound faces per welded edge. Welding tolerance is fixed
at 10 micrometres to account for separate shading corners. This is a closed-edge
topology check; it does not establish absence of geometric self-intersections
or watertight union between separate components. Each catalog must finish its
native checks in less than two seconds, measured internally with steady clock;
compilation, Node startup and authoring are outside that native evaluation budget.
`matter_demo_recipe_types` checks the catalog against the SDK's declarations.

These tests are CTest/Node gates; AQ's `aq test` is a Python/pytest runner and
cannot run them. Use Release for the timed gate. Windows consumers can use
`web/procedural-demo/tests/build-windows.ps1`, which resolves the supported MSVC
toolchain through the repository module. From WSL, run the Node gate against
the resulting executable:

```sh
node web/procedural-demo/tests/recipes.mts web/procedural-demo/tests/build/windows-msvc/release/matter_demo_native_recipes.exe
```
