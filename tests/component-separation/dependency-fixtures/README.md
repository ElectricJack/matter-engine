# Dependency-check fixtures

These are isolated fixtures for `tools/component-separation/check_dependencies.py`. `../test_dependency_checks.py` is the runner.

`base/` is a miniature, clean repository with its own `component-rules.json` and `ownership.json`. It mirrors the shape of the real component split:

| Fixture | Plays the part of |
|---|---|
| `libs/FxContracts` | the neutral contracts (depends on nothing) |
| `libs/FxGeometry` | geometry (contracts only; no JS engine, no renderer) |
| `libs/FxGraphics` | graphics (contracts only; an optional `sdk` variant gated on the undeclared-on-this-machine `fx-sdk` capability) |
| `libs/FxScaffold` | a scaffold component (build file, no code) |
| `packages/fx-authoring` | a portable JavaScript authoring package |
| `integrations/fx-web` | an absent component (must not exist) |
| `legacy/` | in-place legacy code that no component owns |
| `third_party/fakejs`, `fakerender` | a stand-in JS engine and renderer |

`cases.json` lists the cases. For each case, the runner works as follows:

1. It copies `base/` to a temporary directory and applies the case's `edits`. The edit ops are `write`, `append`, `replace` (the old text must occur exactly once), `copy`, `delete`, and `json` with `at`/`set`/`extend`/`remove`.
2. It runs the checker with `args`.
3. It compares the result with `expect`:
   - The exit code must match.
   - The exact set of violation `codes` and the exact set of `failing` result keys must match.
   - The `untested` and `passing` keys must be a subset of the results.
   - The `details` and `notes` fragments must appear, and so must the `stderr` text.
   - When `no_report` is set, the run must not write a report (used for exit-2 rules and usage errors).

Paths in edits are relative to the copied repository. `../external/...` lands beside it and stands in for files outside the checkout.

Cases marked `toolchain` configure or build CMake probes. They need cmake, Ninja and a C++ compiler, and are skipped with the reason otherwise. Everything runs serially with `--jobs 1`.

To add a check, add a violation code to the checker and a case here that provokes it. `test_every_violation_code_is_exercised` fails until both exist. The only exceptions are the two environment-only codes listed in the runner.
