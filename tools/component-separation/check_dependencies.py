#!/usr/bin/env python3
"""Independent component dependency and ownership checks.

Every procedural component must stand on its own: geometry without the
JavaScript host or the renderer, graphics without geometry or terrain, and
the neutral contracts without anything. This tool proves that per component,
from the rules in ``rules.json``:

  static      module ownership, the public-header include closure and the
              JavaScript import surface (no compiler needed)
  duplicates  copies of algorithm code across component boundaries
  configure   configure each component alone (component-off build) and audit
              its targets, link graph, compiled sources, include directories
              and configure-time inputs through the CMake File API
  build       build that probe (one executable that includes every public
              header and links every object of the component) and audit real
              header dependencies (Ninja) and archive symbols

Components the rules mark ``scaffold`` or ``absent``, and variants whose
declared capability is unavailable on this machine, are reported UNTESTED with
the reason, never silently passed.

Exit codes: 0 no violations (UNTESTED results are listed); 1 violations, or
UNTESTED results under --strict-untested, or a --require-capability variant
that could not run; 2 usage, rules or environment error.
"""

from __future__ import annotations

import argparse
import os
import platform
import shutil
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from depcheck.cmake_probe import Probe, Toolchain, cmake_version  # noqa: E402
from depcheck.duplicates import check_duplicates  # noqa: E402
from depcheck.report import FAIL, UNTESTED, Result, print_results, summarize, write_report  # noqa: E402
from depcheck.rules import RulesError, corpus, load_rules  # noqa: E402
from depcheck.static_checks import (  # noqa: E402
    Corpus, check_component_ownership, check_js_imports, check_public_includes, check_repository_ownership, untested)

CHECKS = ("static", "duplicates", "configure", "build")
PROBE_CHECKS = ("configure", "component-off", "link", "sources", "include-dirs", "configure-inputs", "unbuilt-sources")
BUILD_CHECKS = ("build", "headers", "symbols")


def platform_name() -> str:
    if os.name == "nt":
        return "windows"
    return "macos" if sys.platform == "darwin" else "linux"


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", type=Path, default=HERE.parents[1], help="repository root (default: this checkout)")
    parser.add_argument("--rules", type=Path, default=HERE / "rules.json", help="rules file (default: rules.json beside this script)")
    parser.add_argument("--checks", default=",".join(CHECKS),
                        help=f"comma-separated subset of {', '.join(CHECKS)} (default: all; build implies configure)")
    parser.add_argument("--component", action="append", default=[], metavar="NAME",
                        help="check only this component (repeatable; default: every component)")
    parser.add_argument("--list", action="store_true", help="list components, statuses and variants, then exit")
    parser.add_argument("--build-root", type=Path,
                        help="probe build directory (default: <root>/build/component-deps/<platform>)")
    parser.add_argument("--jobs", type=int, default=1, help="build parallelism per probe; probes run one at a time (default: 1)")
    parser.add_argument("--cmake", default=os.environ.get("MATTER_DEPCHECK_CMAKE"), help="cmake executable (env MATTER_DEPCHECK_CMAKE)")
    parser.add_argument("--generator", default=os.environ.get("MATTER_DEPCHECK_GENERATOR"),
                        help="CMake generator (env MATTER_DEPCHECK_GENERATOR; default: Ninja when ninja is on PATH)")
    parser.add_argument("--make-program", default=os.environ.get("MATTER_DEPCHECK_MAKE_PROGRAM"),
                        help="CMAKE_MAKE_PROGRAM, e.g. the resolved ninja (env MATTER_DEPCHECK_MAKE_PROGRAM)")
    parser.add_argument("--build-type", default="Debug", help="CMAKE_BUILD_TYPE of the probes (default: Debug)")
    parser.add_argument("--fresh", action="store_true", help="delete each probe directory (only ones this tool created) first")
    parser.add_argument("--report", type=Path, help="write a JSON report here")
    parser.add_argument("--require-capability", action="append", default=[], metavar="NAME",
                        help="fail instead of reporting UNTESTED when this declared capability is unavailable")
    parser.add_argument("--strict-untested", action="store_true", help="exit 1 when anything is UNTESTED")
    parser.add_argument("--verbose", action="store_true", help="print notes (targets, commands, scan counts)")
    return parser.parse_args(argv)


def resolve_toolchain(args: argparse.Namespace) -> Toolchain:
    cmake = args.cmake or shutil.which("cmake")
    if not cmake or not (Path(cmake).is_file() or shutil.which(cmake)):
        raise RulesError("configure/build checks need cmake: put it on PATH or pass --cmake (MATTER_DEPCHECK_CMAKE)")
    if args.jobs < 1:
        raise RulesError("--jobs must be at least 1")
    generator, make_program = args.generator, args.make_program
    if generator is None and (make_program or shutil.which("ninja")):
        generator = "Ninja"
    version = cmake_version(cmake)
    if not version:
        raise RulesError(f"{cmake} --version failed; is it a working cmake?")
    return Toolchain(cmake=cmake, generator=generator, make_program=make_program, build_type=args.build_type,
                     jobs=args.jobs, fresh=args.fresh, version=version)


def run(args: argparse.Namespace) -> int:
    root = args.root.resolve()
    rules = load_rules(args.rules.resolve(), root)
    checks = [c.strip() for c in args.checks.split(",") if c.strip()]
    unknown = [c for c in checks if c not in CHECKS]
    if unknown or not checks:
        raise RulesError(f"unknown check(s) {unknown or checks}; choose from {', '.join(CHECKS)}")
    selected_checks = set(checks)
    if "build" in selected_checks:
        selected_checks.add("configure")
    names = args.component or list(rules.components)
    missing = [n for n in names if n not in rules.components]
    if missing:
        raise RulesError(f"unknown component(s) {missing}; known: {', '.join(rules.components)}")
    for capability in args.require_capability:
        if capability not in rules.capabilities:
            raise RulesError(f"unknown capability {capability!r}; declared: {', '.join(rules.capabilities) or 'none'}")
    if args.require_capability and "configure" not in selected_checks:
        raise RulesError("--require-capability only applies to the configure/build checks")
    components = [rules.components[n] for n in names]

    if args.list:
        for component in rules.components.values():
            variants = ", ".join(v.name + (f" [needs {v.capability}]" if v.capability else "") for v in component.variants)
            print(f"{component.name:<20} {component.status:<12} {component.kind:<11} {component.directory}  ({variants})")
            if component.status != "implemented":
                print(f"{'':<20} {component.reason}")
        return 0

    toolchain = resolve_toolchain(args) if "configure" in selected_checks else None
    started = time.monotonic()
    files = corpus(rules.repo)
    results: list[Result] = []

    if "static" in selected_checks:
        shared = Corpus(rules, files)
        results.append(check_repository_ownership(rules, names))
        for component in components:
            results.append(check_component_ownership(rules, shared, component))
            static = "js-imports" if component.kind in ("javascript", "browser") else "public-includes"
            if component.status != "implemented":
                results.append(untested(component, static))
            elif static == "js-imports":
                results.append(check_js_imports(rules, shared, component))
            elif component.kind == "native":
                results.append(check_public_includes(rules, shared, component))

    if "duplicates" in selected_checks:
        results.extend(check_duplicates(rules, files, components))
        results.extend(untested(c, "duplicates") for c in components if c.status != "implemented")

    if "configure" in selected_checks:
        build_root = (args.build_root or root / "build" / "component-deps" / platform_name()).resolve()
        build_root.mkdir(parents=True, exist_ok=True)
        compiled: dict[str, set[str]] = {}
        probed = []
        stage_checks = PROBE_CHECKS + (BUILD_CHECKS if "build" in selected_checks else ())
        for component in components:
            if component.kind not in ("native", "adapter"):
                continue  # JavaScript and browser components have no CMake build to probe
            if component.status != "implemented":
                results.extend(untested(component, check) for check in stage_checks)
                continue
            for variant in component.variants:
                print(f"probing {component.name}/{variant.name} ...", file=sys.stderr, flush=True)
                probe = Probe(rules, files, component, variant, toolchain, build_root)
                try:
                    outcome = probe.run(selected_checks, set(args.require_capability))
                except RuntimeError as error:
                    raise RulesError(str(error)) from error
                results.extend(outcome.results)
                for source, owners in outcome.compiled.items():
                    compiled.setdefault(source, set()).update(owners)
                if outcome.compiled:
                    probed.append(f"{component.name}/{variant.name}")
        twice = Result("repository", "-", "compiled-twice")
        for source, owners in sorted(compiled.items()):
            if len(owners) > 1:
                twice.fail("duplicate.compiled-twice",
                           f"{source} is compiled by components {', '.join(sorted(owners))}; one must own it, the others link it")
        twice.notes.append(f"compared {len(compiled)} compiled source(s) across {', '.join(probed) or 'no probes'}")
        results.append(twice)

    print_results(results, args.verbose)
    summary = summarize(results)
    elapsed = time.monotonic() - started
    print(f"\n{summary['pass']} passed, {summary['fail']} failed, {summary['untested']} untested "
          f"({summary['violations']} violation(s)) in {elapsed:.1f}s")
    untested_results = [r for r in results if r.status == UNTESTED]
    if untested_results:
        print("UNTESTED (not a pass):")
        for result in untested_results:
            print(f"  {result.key}: {result.reason}")
    if args.report:
        context = {
            "root": root.as_posix(), "rules": args.rules.resolve().as_posix(), "platform": platform_name(),
            "python": platform.python_version(), "checks": sorted(selected_checks), "components": names,
            "toolchain": toolchain.describe() if toolchain else None, "elapsed_seconds": round(elapsed, 1),
        }
        write_report(args.report, results, context)
    if any(r.status == FAIL for r in results):
        return 1
    if args.strict_untested and untested_results:
        print("failing because --strict-untested was given", file=sys.stderr)
        return 1
    return 0


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        return run(args)
    except RulesError as error:
        print(f"check_dependencies: error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
