# world_demo — scene layout

```
projects/world_demo/
  scenes/<Name>/
      <Name>.js          the scene script (a `class <Name> extends World`)
      objects/*.js       objects ONLY this scene uses
      props.json         authored property overrides (editor-written)
      README.md          optional per-scene notes
  objects/*.js           objects SHARED by two or more scenes
  shared-lib/*.js        importable JS modules (`import ... from 'shared-lib/x'`)
  tests/                 JS-level tests
  .cache/<Name>/         generated; never edit, safe to delete
```

## The one rule

**A file's location is its blast radius.**

* `scenes/<Name>/objects/Foo.js` — changing it can affect `<Name>` and nothing else.
* `objects/Foo.js` — changing it can affect *any* scene in the project. Check
  before you edit. This tier is deliberately small.

Object lookup is a **search path**: the scene's own `objects/` is consulted
first, then the project-wide `objects/`. First match wins, so a scene-local file
**shadows** a project-wide one of the same name.

## Adding an object

Put it in `scenes/<YourScene>/objects/`. That is the default and it is always
safe. Promote it to the project tier only when a second scene actually needs it
— and once promoted, understand you have taken on every scene as a caller.

## WorldSector.js

Every streaming scene has **its own** `scenes/<Name>/objects/WorldSector.js`. It
describes how that one scene's sectors are populated (scatter, biome gates,
density), which is scene-specific by nature — sharing it would couple unrelated
worlds together.

The engine loads it by the fixed *name* `WorldSector`, resolved through the
search path above, so the scene's copy is what runs. `objects/WorldSector.js` is
a **template** that no shipped scene resolves to; it is what a new streaming
scene starts from. Editing the template changes nothing about existing scenes.

## Adding a scene

1. `mkdir scenes/MyScene && $EDITOR scenes/MyScene/MyScene.js`
2. For a streaming scene, `cp objects/WorldSector.js scenes/MyScene/objects/`
   and edit *that* copy.
3. It appears in the editor's world list automatically — the scan walks
   `scenes/*/` and matches `<dir>/<dir>.js`. A folder whose script is missing or
   misnamed is skipped silently, so check the name matches if a scene does not
   show up.

`MatterEngine3/tests/world_definition_tests.cpp` walks this directory and loads
every scene, so a new one is covered without touching the test.

## Legacy flat layout

`worlds/<Name>.js` with no scene folder still resolves (`LocalProviderConfig::
for_project` falls back to it), which is what the sandbox projects built by
`async_bake_tests` / `demand_bake_tests` use. It is the *script* that selects
the layout, not the presence of `scenes/`, so a project can migrate one scene
at a time.
