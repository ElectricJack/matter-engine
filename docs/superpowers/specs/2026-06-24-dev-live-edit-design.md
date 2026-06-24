# SP-5 — Dev / Creative Live-Edit — Design

**Status:** Approved for planning (2026-06-24)
**Project:** MatterSurfaceLib
**Parent:** `2026-06-24-procedural-part-authoring-design.md` (sub-project SP-5)
**Consumes:** SP-2 (`ScriptHost` bake), SP-3 (resolve/cache-miss), SP-4 (flatten +
sector composition).

## Goal

Give an author a tight **edit → see-it-in-world** loop while keeping the install-mode
contract intact. The author edits a part script in their **own editor**; the running engine
watches the files, re-bakes only what changed, and re-composes the affected part of the
world — no restart, no full reinstall.

Dev mode is **the same pipeline as install**, driven by file events and scoped to the
change, with a time budget on bakes so a runaway edit can't hang the session (the budget
SP-2 already supports — unset for install, **set** here).

## Editing model — external editor + file watch

- **No in-app editor.** The author uses their preferred external editor (VS Code, etc.) on
  the `ObjectSchemas/*.js` (and shared-lib) files. The engine never owns the text.
- Rationale: zero editor surface to build/maintain; authors keep their tooling; the file is
  the single source of truth that already drives hashing (SP-3).

## Change detection — OS-native file events

- Watch the script directories with **OS-native file-change notifications**
  (`inotify` on Linux, `ReadDirectoryChangesW` on Windows) — **not** polling.
- Rationale (author override of the initial poll recommendation): native events are
  immediate and cheap, give a responsive loop without a polling interval to tune, and don't
  spin CPU watching an idle tree. Debounce rapid successive saves (editors often write
  twice) into one rebuild.

## Rebuild scope — affected subtree, upward cone

On a file-change event for part `P` (or a shared-lib module `M`):

1. **Re-resolve** the identity of `P` (or every part importing `M`) — recompute its
   `resolved_hash` from the new source (SP-3 resolve). Source change ⇒ new hash.
2. **Rebuild upward:** every **ancestor** of the changed part also gets a new resolved hash
   (their folded child hash changed), so they miss the cache too. Re-bake the **upward
   cone** — the changed part plus all its ancestors up to the root — in topo order. Sibling
   branches and unaffected leaves stay cache hits (untouched).
3. **Re-flatten the affected subtree:** re-run SP-4 flatten for the portion of the world
   under the affected root, regenerating the per-sector instance lists / LOD'd BLASes for
   what changed. Unaffected sectors are left as-is.

This is exactly install's transitive-invalidation behavior (SP-3), but **triggered
incrementally** by a file event and **scoped** to the upward cone + affected subtree rather
than re-running the whole world.

```
file event on P
  → re-resolve P (new hash)  → ancestors A1..An get new hashes
  → bake (P, A1..An) missing artifacts in topo order   [time-budgeted]
  → SP-4 re-flatten subtree under affected root → refresh those sectors' instances/BLAS
  → world updates live; rest of world untouched
```

## Time budget (dev)

- Each bake runs under SP-2's **configurable time budget**, **set** in dev mode so a
  pathological edit (huge grid `forEach`, runaway loop) aborts with a structured error
  surfaced to the author instead of freezing the session. Install mode leaves it unset.

## Error surfacing

- A failed bake (script throw, session misuse, budget exceeded) is **fail-closed** (SP-2):
  the old artifact stays in place, the world keeps showing the last good version, and the
  structured error (message + best-effort source location) is surfaced to the author
  (console/overlay). The author fixes and saves again; the next event retries.

## Relationship to install

| | Install (SP-3) | Dev live-edit (SP-5) |
|---|---|---|
| Trigger | explicit world install | OS file-change event |
| Scope | full reachable graph | changed part's upward cone + affected subtree |
| Bake budget | unset (unbounded) | set (abort runaways) |
| Bake/cache mechanism | same (`ScriptHost` + cache-miss) | same |
| Failure | install fails | last-good kept, error shown, retry on next save |

Dev mode adds **no new bake/cache semantics** — it reuses SP-2/SP-3/SP-4 and adds a watcher
+ incremental scoping + a budget.

## Testing

Headless `tests/dev_live_edit_tests` (simulate file events; GL-free where possible):

- **Single-part edit:** change a leaf → only that leaf + its ancestors rebake; siblings stay
  hits; affected sectors re-flatten, others untouched.
- **Shared-module edit:** change a shared-lib module → every importing part (and their
  ancestors) rebakes; non-importers untouched (depends on SP-7 hash-fold).
- **Debounce:** two writes within the debounce window → one rebuild.
- **Fail-closed retry:** an edit that throws keeps the last-good artifact and reports the
  error; a subsequent valid edit succeeds and updates the world.
- **Budget abort:** an edit that exceeds the dev time budget aborts with a structured error,
  last-good kept.
- **Scope correctness:** the set of rebaked parts equals exactly the upward cone of the
  changed file (instrument bake calls).

## Goals / Non-goals

**Goals**
- External-editor workflow + OS-native file watching (inotify / ReadDirectoryChangesW),
  debounced.
- Incremental re-resolve + upward-cone rebake + affected-subtree re-flatten, reusing
  SP-2/3/4.
- Dev time budget set; fail-closed with last-good retained and errors surfaced.

**Non-goals (deferred)**
- In-app code editor / live-coding REPL.
- Hot-swapping individual BLASes without re-flatten (re-flatten of the affected subtree is
  the v1 mechanism).
- Per-keystroke / on-type rebuild (rebuild is on save/file-event).
- Multi-user / collaborative editing, undo history, asset hot-reload beyond part scripts.
- Parallel rebake (inherits SP-3's single-threaded bake for v1).

## Open questions (resolve in planning)

- Debounce window length and coalescing policy for bursts of saves across multiple files.
- How the affected root for re-flatten is determined when an edited part is instanced under
  multiple roots (re-flatten each affected root's subtree).
- Watcher coverage for newly *created* / *deleted* / *renamed* script files (not just
  content edits) and how those map to add/remove instances.
- Error surface mechanism for a headless vs. windowed session (console vs. on-screen
  overlay) — likely both, behind one structured-error sink.
