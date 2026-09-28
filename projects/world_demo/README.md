# world_demo — scenes and shared objects

Scenes and objects are grouped by purpose. Folder names organize the browser;
**scene names and object module names stay unchanged**. For example,
`MATTER_WORLD=ClayBrickMaze` and `placeChild('ClayBrickWallSurface', ...)` work
regardless of the grouping folders.

```text
projects/world_demo/
  scenes/<category>/[<family>/]<Name>/
    <Name>.js          scene script (`class <Name> extends World`)
    objects/          scene-local objects; optional grouping folders
    props.json        authored settings, saved by the editor
    README.md         optional scene notes
    capture.*         optional capture helpers
  objects/<family>/[<group>/]*.js   shared object modules
  shared-lib/*.js                   importable JS libraries
  tests/                           JS checks
  .cache/<Name>/                   generated cache, keyed by scene name
```

## Scenes

| Folder | Contents |
| --- | --- |
| [texturing/bricks](scenes/texturing/bricks/) | Brick geometry, material and wall proofs, POM brick proof, brick maze |
| [texturing/materials](scenes/texturing/materials/) | Metal and tileset examples |
| [texturing/pom](scenes/texturing/pom/) | General parallax occlusion mapping proof |
| [texturing/terrain](scenes/texturing/terrain/) | Procedural terrain material proof |
| [texturing/virtual_texture](scenes/texturing/virtual_texture/) | Chart VT and seam proofs |
| [architecture/villa](scenes/architecture/villa/) | Villa Doric column study, website kit and gold treasury |
| [castles/layouts](scenes/castles/layouts/) | Castle layouts, assemblies, galleries and Kreuzenstein |
| [castles/materials](scenes/castles/materials/) | Masonry and material studies |
| [castles/furnishings](scenes/castles/furnishings/) | Furnishings gallery |
| [castles/proofs](scenes/castles/proofs/) | Bake, connector, timber and walk probes |
| [vegetation](scenes/vegetation/) | Trees, branches, forests, meadow and vegetation galleries |
| [terrain](scenes/terrain/) | Rock gallery |
| [streaming](scenes/streaming/) | Streaming mountains, caverns and meadow |
| [lighting](scenes/lighting/) | Cornell box, light galleries and isolated light fixtures |
| [atmosphere](scenes/atmosphere/) | Clouds, fog and atmosphere fixtures |
| [water](scenes/water/) | River hydrology and floating objects |
| [physics](scenes/physics/) | Physics playground |
| [examples](scenes/examples/) | General demo, floor and animation gallery |

## Shared objects

| Folder | Contents |
| --- | --- |
| [texturing/bricks](objects/texturing/bricks/) | Clay brick sources, surfaces and wall generators |
| [architecture/villa](objects/architecture/villa/) | Villa website-kit parts and the Doric column, named by `shared-lib/villa_*.js` |
| [castle](objects/castle/) | `materials/`, `structure/`, `furnishings/`, `fixtures/` |
| [vegetation](objects/vegetation/) | `alpine/`, `conifer/`, `trees/`, `groundcover/` |
| [terrain](objects/terrain/) | Rocks, scree, pebbles and snow |
| [props](objects/props/) | Shared props and playground floor |
| [lighting](objects/lighting/) | Shared lighting fixture |
| [templates](objects/templates/) | WorldSector starting template |

The Assets pane mirrors these folders. Objects are separated into **Shared**
and **Scenes**, with each scene's own objects under its scene folder. Filtering
matches folder names as well as scene/object names.

## Ownership and lookup

A scene-local object affects only its owning scene. Shared objects can affect
any scene that uses them. Put new objects in the scene's `objects/` folder by
default; promote them to the appropriate shared family when another scene needs
them.

Object lookup searches recursively within the scene's `objects/` tier, then the
project's `objects/` tier. A scene-local module shadows a shared module with the
same name. **Module filename stems must be unique within each tier.** Two
`Rock.js` files in separate shared groups are an error; a local `Rock.js` and a
shared `Rock.js` are allowed. Scene names must also be unique across groups.
Hidden folders and directory symlinks are excluded from discovery.

`shared-lib/` remains flat; imports such as `shared-lib/clay_brick_source`
are unchanged. Scene-local objects, settings and helpers move with their scene.

## Adding a scene

1. Pick a category and create `scenes/<category>/MyScene/MyScene.js`.
2. For a streaming scene, copy [objects/templates/WorldSector.js](objects/templates/WorldSector.js)
   into that scene's `objects/` folder and edit the copy.
3. The editor discovers any `<Name>/<Name>.js` below `scenes/`. It stops at a
   recognized scene folder, so scene-local objects and helpers cannot become
   accidental scenes.
4. Keep capture helper repository-root paths correct if adding folder depth.

The engine's `world_definition_tests` loads every discovered scene. Node tools
use [tools/project_layout.mjs](../../tools/project_layout.mjs) to resolve grouped
scenes and objects rather than rebuilding paths by hand.

Legacy `scenes/<Name>/<Name>.js` and `worlds/<Name>.js` projects still work.
The scene script takes precedence over a legacy world with the same name.
Cache directories remain `.cache/<Name>/`; moving source paths can invalidate
resolve metadata once, while content-addressed baked assets keep their identity.
