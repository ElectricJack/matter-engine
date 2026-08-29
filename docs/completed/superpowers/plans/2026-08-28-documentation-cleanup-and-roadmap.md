# Documentation Cleanup and Roadmap Implementation Plan

**Status:** Completed 2026-08-28

> **Execution note:** Keep existing source changes untouched. Stage and commit
> only the documentation files named or moved by this plan.

**Goal:** Reduce the design archive to current unfinished authority, preserve
implemented history separately, retire superseded decisions, and replace the
legacy roadmap with the approved raster-water direction and core remaining
work.

**Source of truth:**
`docs/findings/spec-implementation-gap-audit-2026-08-28.md` contains the exact
201-document current-tree ledger. The approved lifecycle and renderer policy
is `docs/superpowers/specs/2026-08-28-render-eligibility-and-document-lifecycle-design.md`.

**Classification rule:**

- audit class `SUPERSEDED/OBSOLETE` moves to `docs/deprecated/`;
- audit class `IMPLEMENTED` moves to `docs/completed/`, except the five
  canonical architecture documents listed in Task 2;
- `PARTIAL` and `MISSING` remain active unless Task 3 explicitly retires or
  replaces them;
- old-branch-only documents are not imported; and
- no document is deleted.

---

## Task 1: Create lifecycle destinations

**Create:**

- `docs/completed/README.md`
- `docs/completed/MANIFEST.md`
- `docs/deprecated/README.md`
- `docs/deprecated/MANIFEST.md`

The completed README must state that these are landed implementation records,
not the current backlog, and that source plus canonical architecture docs win
if behavior changed. The deprecated README must state that its files are not
implementation authority and are ready for later removal after a reference
audit.

Both manifests record original path, new path, classification reason, and a
successor where the audit identifies one.

**Verify:**

```powershell
Get-Content docs/completed/README.md
Get-Content docs/deprecated/README.md
```

## Task 2: Move audited completed and deprecated records

Use the complete-inventory tables in the gap audit to construct a move list.
Before any move, resolve every source and destination and assert both remain
inside the worktree. Preserve the original relative category below the
lifecycle root:

```text
docs/superpowers/plans/X.md
  -> docs/completed/superpowers/plans/X.md

docs/superpowers/specs/X.md
  -> docs/deprecated/superpowers/specs/X.md

MatterEngine3/docs/X.md
  -> docs/<lifecycle>/MatterEngine3/X.md
```

Move all 52 current-tree `SUPERSEDED/OBSOLETE` documents to deprecated.
Move all 125 `IMPLEMENTED` documents to completed except these current
canonical architecture documents, which remain active:

- `docs/contour-seam-design-2026-08-13.md`
- `docs/volumetric-sectors-design-2026-08-10.md`
- `MatterEngine3/docs/event-system.md`
- `MatterEngine3/docs/part-workbench.md`
- `MatterEngine3/docs/vulkan-rt-gtex-bake.md`

Also move `docs/superpowers/REVIEW-LOG.md` to
`docs/completed/superpowers/REVIEW-LOG.md` as process history.

Expected audited outcome before Task 3: 120 completed moves, 52 deprecated
moves, five implemented canonical documents left active, and all 24 audited
partial/missing/backlog documents still present.

**Verify:**

```powershell
Get-ChildItem docs/completed -Recurse -File | Measure-Object
Get-ChildItem docs/deprecated -Recurse -File | Measure-Object
Test-Path MatterEngine3/docs/event-system.md
Test-Path docs/contour-seam-design-2026-08-13.md
```

## Task 3: Reconcile water policy and retire replaced queues

**Move to deprecated:**

- `docs/superpowers/plans/2026-08-24-real-time-river-presentation-floating-bodies.md`
- `docs/superpowers/specs/2026-08-24-real-time-river-presentation-floating-bodies-design.md`
- `docs/superpowers/backlog.md`
- `docs/superpowers/plans/2026-07-26-procedural-animation-remaining-work.md`

The first pair is superseded by the general render-eligibility/raster-water
design. The backlog pair is replaced by the concise roadmap; retained ideas
must appear under deferred decisions rather than vanish.

Before moving the implemented baked-water animation design to completed, add
a prominent historical note that its animated-BLAS section is superseded by
the 2026-08-28 raster-only water policy. Do not rewrite the historical
implementation description as if the earlier decision never happened.

Update `docs/findings/spec-implementation-gap-audit-2026-08-28.md` so it no
longer calls raster-only water a proposal under consideration. Record the
approved decision and treat removing animated-water BLAS/TLAS work plus
improving raster-water quality as current implementation gaps.

Move the old root roadmap content to:

- `docs/deprecated/roadmaps/ROADMAP-legacy-2026-08-28.md`

Then create a new root `ROADMAP.md` in Task 4.

## Task 4: Write the concise current roadmap

**Replace:** `ROADMAP.md`

Keep the document short and ordered:

1. **Now — raster water and render eligibility**
   - part and instance `rayTraced` boolean with instance precedence;
   - water authored raster-only;
   - no animated-water BLAS cache or TLAS instance;
   - refraction, shallow-visible depth fog, turbulence foam, flow animation,
     reflections, correct shadows;
   - matched screenshots and frame/memory evidence.
2. **Next — playable river proof**
   - reliable longer sequential river sections and handoffs;
   - rapids traversal using floating craft;
   - clean MSVC/PhysX bake, cache-hit, collision, and playback acceptance.
3. **Scale before expansion**
   - stable-slot/O(changed) TLAS CPU mirror;
   - finish app-lane publish-tail removal;
   - close the LOD/VT proxy, visibility, and unified-budget endpoint;
   - memoize dynamic command layout if profiling confirms it remains material.
4. **Deferred decisions, not commitments**
   - whether animated models should default to raster-only;
   - integrate the completed character-controller branch for a character
     playtest;
   - grouped authoring/editor, animation, and material ideas retained from the
     old backlog.
5. **Explicitly retired**
   - bespoke LBM river solvers, legacy GL/Explorer paths, CUDA/OptiX renderer
     paths, old impostor generations, animated RT water, and runtime fluid
     interaction.

Link the gap audit and approved design instead of copying their full detail.

## Task 5: Rebuild documentation indexes and references

**Modify:**

- `docs/README.md`
- `MatterEngine3/docs/README.md`
- every active Markdown file containing an old path to a moved document

The top-level index must expose:

- current roadmap;
- current active designs and gap audit;
- canonical agent/runbook docs;
- completed and deprecated lifecycle directories; and
- a short explanation that dated plans are no longer a flat searchable
  archive.

The engine index must keep canonical architecture documents in place and stop
describing the renderer as ray-tracing every object unconditionally. It may
describe authored raster-only eligibility as approved/planned until code lands.

Update active links to current successors. Historical documents may retain
links to other moved history, but those links must resolve to the new paths.

## Task 6: Validate the cleaned set

Run:

```powershell
git diff --check
rg -n "docs/superpowers/(plans|specs)/" ROADMAP.md docs/README.md MatterEngine3/docs/README.md docs --glob "*.md"
git status --short
```

Then perform a local Markdown-link existence scan over `ROADMAP.md`,
`docs/**/*.md`, and `MatterEngine3/docs/**/*.md`. Ignore URLs and anchors;
every relative filesystem target in an active document must exist.

Confirm:

- no source/build/project file changed as part of this cleanup;
- no document was deleted rather than moved;
- the current roadmap contains no LBM, legacy GL, or animated-water BLAS task;
- the approved design remains in active specs;
- partial/missing documents not explicitly retired remain available; and
- manifests account for every moved document.

Stage only the roadmap, audit/design/index changes, lifecycle manifests, and
the detected document renames. Commit them as one documentation-lifecycle
change after verification.
