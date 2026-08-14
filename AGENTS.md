# AGENTS.md

MatterEngine2 is a single git monorepo of independently-buildable C/C++
sub-projects (a procedural voxel/particle engine and its Vulkan editor) plus
the JS world-authoring layer they run. This file is a pointer, not a second
copy of anything below — read the linked docs, don't infer from this one.

- **[CLAUDE.md](CLAUDE.md)** — the canonical agent onboarding doc: repo
  layout, project relationships/dependency order, the build toolchain
  (MSYS2/UCRT64, `platform.mk`, per-project Makefiles), worktree setup, and a
  QA quick-reference. Written for Claude but not Claude-specific; start here
  regardless of which agent/tool you are.
- **[docs/agent/](docs/agent/)** — the engine's external control surface for
  scripts/agents: `control-surface.md` (env vars + the `MATTER_CMD_FIFO`
  command grammar + events), `qa-cookbook.md` (copy-paste recipes: builds,
  screenshots, FIFO sessions, replay/diff, smoke/seam/perf gates), and
  `issue-system.md` (the in-editor capture/file/replay pipeline).
- **[docs/README.md](docs/README.md)** — index of every other doc in `docs/`:
  design docs, findings/measurements, runbooks, and baselines.
- **[MatterEngine3/docs/](MatterEngine3/docs/)** — engine-internal
  architecture: the bake pipeline, part-graph/determinism, the renderer's
  per-frame composition, and the event-system design.

This file intentionally duplicates nothing above — if a fact here ever
disagrees with one of those docs, the linked doc wins.
