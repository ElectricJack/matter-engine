# Castle system task register

Specification: [Design](castle-grid-and-local-lighting.md).

Astra owns final assembly; its two queue tasks are paused to reserve them for this conversation, not because user input is needed.

## clear-apex: Castle kit: grid plans and reusable architectural components

- `clear-apex.1` — Compile metre-grid floor plans and matching wall sockets. Dependencies: none.
- `clear-apex.2` — Create detailed voxel stone, timber primitives and PBR materials. Dependencies: none.
- `clear-apex.3` — Assemble wall modules, openings, corners and curved masonry. Dependencies: clear-apex.1, clear-apex.2.
- `clear-apex.4` — Build grid floors, connected timber, stairs and roofs. Dependencies: clear-apex.1, clear-apex.2.
- `clear-apex.5` — Create furnished interiors, gold/glass details and light fixtures. Dependencies: clear-apex.2.

## swift-flare: Castle lighting: many point/spot lights in raster and ray-traced GI

- `swift-flare.1` — Connect authored local lights and build a shared spatial light index. Dependencies: none.
- `swift-flare.2` — Render hundreds of local lights through deferred raster composition. Dependencies: swift-flare.1.
- `swift-flare.3` — Add ray-traced local shadows and local-light bounce radiance. Dependencies: swift-flare.2.

## bold-glacier: Castle showcase: Astra assembly and visual acceptance

- `bold-glacier.1` — Astra: assemble three furnished castle plans and gallery. Dependencies: clear-apex, swift-flare.
- `bold-glacier.2` — Astra: validate walkthroughs, ray-traced interiors and many-light behavior. Dependencies: bold-glacier.1.

Live status: `aq task list --project matter-engine-cpp`.

Source graph documents and creation/validation responses: `build/qa/castle-grid/`.

## Delegation and provider allocation

Workers are explicitly encouraged to use subagents for independent work.
`clear-apex.3` and `.4` use Claude standard-high; `.5` uses Claude
standard-medium. The other seven leaf tasks (including Astra integration)
remain Codex: a 70/30 split by task count.

Scheduling refinement: furniture task `clear-apex.5` consumes the already pushed
primitive API at `7facdb36` while `clear-apex.2` completes additional visual
captures. Its blocking edge became a related edge; this releases independent
implementation without declaring the upstream visual acceptance complete.

Masonry and structure were also released against frozen compiler ac73a2a5 and primitive 7facdb36: clear-apex.2 links are related while final fixture visual polish continues. No final visual acceptance is waived.
