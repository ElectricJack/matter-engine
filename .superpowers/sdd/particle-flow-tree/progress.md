# Particle-flow tree SDD progress ledger
Plan: docs/superpowers/plans/2026-07-09-particle-flow-tree.md
Spec: docs/superpowers/specs/2026-07-09-particle-flow-tree-design.md
Branch: worktree-particle-flow-tree (worktree .claude/worktrees/particle-flow-tree)
Start (BASE for final review): 292651a (plan commit)

## Completed tasks

## Minor findings roll-up (for final review triage)
Task 1: complete (commits 292651a..ff9781d, review approved after fix)
- Controller decision (surface to Jack): plan's XOR spatial-hash key replaced with injective packed-coordinate key (ff9781d) — plan's example code conflicted with its own collision-safety/determinism constraints; reviewer finding, fix verified by re-review
- Minor (T1): unused <cstring> include in pf_tests.cpp (plan artifact)
- Minor (T1): PathSet channel_names "fixed at construction" documented but not structurally enforced — Task 2+ should uphold
Task 2: complete (commits ff9781d..9051d63, review approved; Important const_cast finding fixed in 9051d63, re-review approved)
- Minor (T2): emitter cap zeroes fractional emission credit instead of deferring it (pf_sim.cpp emit path)
- Minor (T2): forward decl of V3 in pf_spatial_hash.h unnecessary; exact-zero float compare post-normalize in steer path (redundant but safe)
Task 3: complete (commits 9051d63..3c4afb2, review approved; brief-mandated const_cast in field_force removed in 3c4afb2, controller-verified)
- Minor (T3): separate_dir's n counter redundant (normalize is zero-safe)
- Minor (T3): adhere_dir computes `out` vector even when surface_offset==0 (wasted ops, correct result)
Task 4: complete (commits 3c4afb2..d5c8064, review approved, CHECKPOINT A PASS 15/15 ASan/UBSan clean)
- Deferred-Important (T4, controller decision): attract-killed particles record start-of-tick position as death vertex (pf_sim.cpp sequencing, plan-mandated code; no spec statement requires capture-point; impact <= one step length). Final review to triage.
- Minor (T4): min_seg_==0 suppresses all mid-path vertices (latent, unexercised)
- Minor (T4): known_[id] not cleared after path close (unreachable given monotonic ids)
Task 5: complete (commits d5c8064..bf3021e, review approved; 5 brief-vs-kernel deviations all verified correct: run(chunk) vs private step(), vel0 scalar, Fade::axis V3, kill_on_consume=true, JS_NewTypedArray argc=3 UB fix)
- Minor (T5): FieldConfig::seed cast to uint64_t but member is uint32_t (silent narrowing) — fix before production
- Minor (T5): argd/get_num discard JS_ToFloat64 error return (pattern from brief)
- Minor (T5): j_pf_path doesn't check f32_copy for JS_EXCEPTION (OOM path)
- Minor (T5): JS emitter shape default = point (0) vs kernel default disc (1) — deliberate ergonomic choice
- Note (T5): worktree lacked libraylib.a; symlinked from main repo (infrastructure)
Task 6: complete (commits bf3021e..b3edc34, spec approved; Important SoA-reallocation-under-live-views hazard fixed by ctor pre-reserve in b3edc34, controller-verified diff; adaptations run(chunk)/axis*vel0/argc=3 all reviewer-verified)
- Minor (T6): build_tick_view implicit uint32->size_t widening (safe)
- Minor (T6): every>RUN_CHUNK widens budget-check window (inherent to every-N API, matches brief)
Task 7: complete (commits b3edc34..2e1896b, review approved, CHECKPOINT B PASS: pf 15/15 + run-script ALL PASS + run-partv2 all passed; 6 adaptations reviewer-verified)
- Minor (T7): session-guard fires before arity guard in stampPaths (correct policy, noted)
- Minor (T7): Part.paths placed after position() not endVoxels() (cosmetic)
- WARNING for T8/T9 dispatches: plan briefs show emitter vel0 as array [0,v,0]; binding parses SCALAR (array coerces to 0 → zero-velocity emitter). Use scalar vel0 + V3 axis.
Task 8: complete (commits 2e1896b..c6db01d, review approved; vel0 scalar erratum fixed as pre-warned)
- Minor (T8): maxThickness skip branch untested (test uses maxThickness:10); coneCloud untested until Task 9; lerp3 actually nlerp (naming)
Task 9: complete (commits c6db01d..bfee0e9, review approved; bake gate PASS hash 10d3d6e6750d0faf 15032 tris + double-bake cache hit; visual gate PASS, controller viewed corner+midfield screenshots directly: coherent gnarled trunks, grounded, crown divergence, no blobs)
- Important-note (T9): TREEBAKE_CPP PF sources propagate via sh_CPP_SRCS sort-union to ALL sh-flavor binaries (fixes latent unresolved install_pf_bindings across MEADOW/GALLERY/etc) — implicit coupling if Makefile restructured
- Minor (T9): stale "expect tris=0" comment in tree_bake_check.cpp (pre-existing, untouched)
- Minor (T9): PF_LIB variable vs inline path style divergence in tests/Makefile
Task 10: complete (commits bfee0e9..4134583, review approved after 2 fix rounds; FINAL GATE PASS: build-all 11/11, Windows viewer.exe w/ all 5 pf objects, full sweep green ex known run-asyncbake, correctness checklist all evidenced)
- Fix history (T10): incremental-equivalence test vacuous twice (v1 discarded comparison; v2 size-compare of EMPTY parts) → rewritten 4134583 as one-bake two-sim exact in-JS compare (genuine)
- Minor (T10): append-only mid-check silently skipped if pcMid==0 (practically always >0)
- Minor (T10): budget test maxAge:0 semantics unexamined (budget still genuinely exceeded)
- Minor (T10): script_host_tests not ASan-built (accepted gap; kernel ASan coverage complete)
- Note (T10): 2e7a394 commit intermediate (weakened assertion) superseded by 4134583
## All 10 tasks complete — next: final whole-branch review (opus, 292651a..HEAD)

## Final whole-branch review (opus, 292651a..4134583)
Verdict: Needs fixes — C1 (Critical) tests/Makefile link graph: install_pf_bindings unresolved in ~13 binaries (sort-union only makes compile rules, not link inputs); I1 (Important) pf_bindings.cpp seed cast uint64_t→uint32_t. Attract-kill vertex deferral concurred (documented behavior). Minors M1-M10 + I2 = follow-ups.

Fix: ONE fixer (sonnet) → 57fc79e. PF_CPP hoisted before GRAPH_INT_CPP with comment; added to GRAPH_INT/EXAMPLE/TILESETBAKE/SHLIB/RETOPO_INT/GPU_PIPELINE (derivatives inherit via filter-out); TREEBAKE explicit $(PF_CPP) removed (dedup — obj_list does NOT sort). I1 cast fixed. Verification: meadow/gallery/shlib/viewer-logic/grasslod/stressforest/tilesetbake/liveprod/script/partv2/treebake ALL PASS.
Controller verified diff directly (Makefile + cast correct).

Open: run-example (load_v2 Tree part) + run-graph-integration (Tree/Trunk/TreeBranch/Leaf fixture asserts a three-part demo; no Trunk.js exists in schemas even at 292651a) fail — fixer claims pre-existing; controller confirming on main cf04f03 (run in progress).

Controller verification of the two open failures (2026-07-09): BOTH pre-existing on main cf04f03.
- run-graph-integration: identical 10 FAIL lines on main (test asserts a three-part Tree->Trunk+TreeBranch demo; no Trunk.js exists in world_demo schemas on main or branch base).
- run-example: fails on main too (save_v2 fopen errno=2 in /tmp sandbox AND recomputed resolve_hash b3f796b0... != installed hash 4dcc44be... — same hash-authority divergence the branch shows a9e1b2ce... != 10d3d6ef...). Not caused by this branch.
Follow-up (report to Jack): resolve_hash(src,"{}") vs PartGraph install resolved-hash divergence breaks run-example on main; graph-integration demo fixture stale.

Final review verdict satisfied: C1+I1 fixed (57fc79e), all 13 binaries link, 8/10 previously-broken suites pass, 2 pre-existing failures confirmed on main. Task 10 + final review COMPLETE. Next: finishing-a-development-branch (Windows rebuild first — I1 touched pf_bindings.cpp).
