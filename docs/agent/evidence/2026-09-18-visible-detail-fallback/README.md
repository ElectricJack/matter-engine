# Visible detail diagnostics: traversal-limited fallback

Previous turn added GPU feedback status bit 1 for visible refinement fallback.
This turn found the CPU reference and indexed selectors also omitted fallback
counts when node/depth budget was exhausted. Both now count unresolved requested
refinement in that case; emitted coverage and missing-page requests are unchanged.
This matters because all page requests can finish while a traversal budget still
prevents reaching the requested detail. Empty queues are not sufficient evidence.

Added native coverage assertion for budget-limited cuts with zero requests and
reference/indexed fallback-count equivalence under limits. Added an independent
GPU fixture with seven resident nodes and max_nodes=1: expected two selected
coarse children, zero requests, two unresolved fallback groups. GPU execution is
pending while the user reviews editor PID 28296; it was verified responsive at
turn start. Do not replace that executable or run competing GPU validation.

Remaining: GPU tests, matching retired GPU feedback to camera/scene revisions,
VT required-page completeness and a visible-set acceptance predicate. Existing
global readiness audit remains unchanged. These diagnostics do not establish the
one-second target or sustained sub-10ms rendering.

Native geometry_hierarchy_tests ALL PASS (/tmp/visible-fallback-tests.log).
Build initially failed LNK1104 opening matter_engine_headless.lib; verified no
compiler/linker processes remained and sufficient disk space, then rebuilt
successfully (/tmp/visible-fallback-build-retry.log). Python audit syntax passed.
Review editor PID 28296 remained responsive after tests. GPU fixture remains
unexecuted and the current editor does not contain this turn's CPU-counter edits.
