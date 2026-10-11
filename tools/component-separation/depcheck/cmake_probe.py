"""Configure (and optionally build) one component alone and audit the result.

Each probe is a throwaway CMake project that adds only the component's own
directory, plus an executable that includes every public header and links
every object of the component's libraries. Nothing else is switched on, so
anything that appears in the probe was pulled in by the component itself.
The CMake File API (codemodel-v2, cmakeFiles-v1) supplies the target graph,
sources, include directories, link lines and configure inputs; the build
stage adds the real header dependencies (``ninja -t deps``) and the symbols
of the component's archives.
"""

from __future__ import annotations

import fnmatch
import json
import os
import re
import shlex
import shutil
import subprocess
from dataclasses import dataclass, field
from pathlib import Path

from .report import Result
from .rules import Component, Rules, Variant, HEADER_SUFFIXES, SOURCE_SUFFIXES, glob_match, under

PROBE_TARGET = "matter_component_probe"
# Generator bookkeeping targets (Visual Studio / Xcode); they own no code.
GENERATOR_TARGETS = {"ALL_BUILD", "ZERO_CHECK", "INSTALL", "RUN_TESTS", "PACKAGE", "ALL_CHECK"}
MARKER = ".matter-component-depcheck"
# Every buildsystem target the probe configured, with its TYPE. The File API omits
# INTERFACE libraries without sources (header-only targets), so this list fills them in.
TARGET_LIST = "matter-probe-targets.txt"
OBJECT_LINE = re.compile(r"^(\S.*?): #deps \d+")
OBJECT_TARGET = re.compile(r"(?:^|/)CMakeFiles/([^/]+)\.dir/")
OUTPUT_TAIL = 40


@dataclass
class Toolchain:
    cmake: str
    generator: str | None
    make_program: str | None
    build_type: str
    jobs: int
    fresh: bool
    version: str = ""

    def describe(self) -> dict:
        return {"cmake": self.cmake, "cmake_version": self.version, "generator": self.generator or "(cmake default)",
                "make_program": self.make_program, "build_type": self.build_type, "jobs": self.jobs}


@dataclass
class ProbeOutcome:
    results: list[Result] = field(default_factory=list)
    compiled: dict[str, set[str]] = field(default_factory=dict)  # repo source -> components compiling it


def audited(rel: str, entries: list[tuple[str, str]]) -> str | None:
    """Reason of the first audited entry (file, directory prefix or glob) covering ``rel``."""
    for path, reason in entries:
        if any(ch in path for ch in "*?[") and fnmatch.fnmatchcase(rel, path):
            return reason
        if under(rel, path):
            return reason
    return None


def note_audits(result: Result, label: str, hits: dict[str, set[str]]) -> None:
    """One note per audited exception that was relied on, so a PASS shows what it tolerated."""
    for reason, items in sorted(hits.items()):
        ordered = sorted(items)
        sample = ", ".join(ordered[:3]) + (f" and {len(ordered) - 3} more" if len(ordered) > 3 else "")
        result.notes.append(f"audited {label}: {sample} ({reason})")


def _tail(text: str) -> str:
    lines = [line for line in text.splitlines() if line.strip()]
    return "\n".join(lines[-OUTPUT_TAIL:])


def _run(command: list[str], cwd: Path | None = None) -> tuple[int, str]:
    try:
        completed = subprocess.run(command, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    except OSError as error:
        return 127, f"cannot run {command[0]}: {error}"
    return completed.returncode, completed.stdout.decode("utf-8", "replace")


def cmake_version(cmake: str) -> str:
    code, output = _run([cmake, "--version"])
    return output.splitlines()[0].strip() if code == 0 and output else ""


class Probe:
    def __init__(self, rules: Rules, files: list[str], component: Component, variant: Variant,
                 toolchain: Toolchain, build_root: Path):
        self.rules, self.files, self.component, self.variant = rules, files, component, variant
        self.toolchain = toolchain
        self.dir = build_root / f"{component.name}-{variant.name}"
        self.src = self.dir / "src"
        self.build = self.dir / "build"
        self.owners = rules.target_owner()
        self.allowed = [component.name] + rules.closure(component.name)
        self.targets: dict[str, dict] = {}
        self.id_to_name: dict[str, str] = {}
        self.inputs: list[dict] = []
        self.compiled_paths: set[str] = set()  # normcased absolute paths of every compiled source

    # ------------------------------------------------------------------ paths

    def classify(self, raw: str, base: Path | None = None) -> tuple[str, str | None]:
        """('build' | 'probe' | 'repo' | 'external', repository-relative path or None)."""
        path = Path(raw)
        if not path.is_absolute():
            path = (base or self.src) / path
        path = Path(os.path.normpath(str(path)))
        for kind, root in (("build", self.build), ("probe", self.src)):
            try:
                path.relative_to(root)
                return kind, None
            except ValueError:
                if os.path.normcase(str(path)).startswith(os.path.normcase(str(root)) + os.sep):
                    return kind, None
        rel = self.rules.repo.rel(path)
        return ("repo", rel) if rel is not None else ("external", path.as_posix())

    def public_dirs(self, name: str) -> list[str]:
        """Directories a consumer of ``name`` may see: its own and its closure's public surface."""
        dirs = []
        for current in [name] + self.rules.closure(name):
            component = self.rules.components[current]
            dirs.extend(component.public_include_dirs)
            dirs.extend(component.public_include_roots)
        return dirs

    # ------------------------------------------------------------------ probe project

    def probe_headers(self) -> list[str]:
        other_variant_only = set()
        mine = set()
        for variant in self.component.variants:
            for header in variant.public_headers:
                rel = header if under(header, self.component.directory) else f"{self.component.directory}/{header}"
                (mine if variant is self.variant else other_variant_only).add(rel)
        headers = []
        for directory in self.component.public_include_dirs:
            for rel in self.files:
                if under(rel, directory) and rel.lower().endswith(HEADER_SUFFIXES):
                    if rel in other_variant_only and rel not in mine:
                        continue
                    headers.append((rel[len(directory) + 1:], rel))
        return sorted(set(spec for spec, _ in headers))

    def probe_targets(self) -> list[str]:
        names = list(self.component.targets) + list(self.component.optional_targets) + list(self.variant.targets)
        return list(dict.fromkeys(names))

    def write_project(self) -> None:
        self.src.mkdir(parents=True, exist_ok=True)
        (self.dir / MARKER).write_text("generated by tools/component-separation/check_dependencies.py\n", encoding="utf-8")
        component_dir = self.rules.repo.abs(self.component.directory).as_posix()
        lines = [
            "# Generated by tools/component-separation/check_dependencies.py; do not edit.",
            "cmake_minimum_required(VERSION 3.25)",
            "project(matter_component_probe LANGUAGES C CXX)",
            f'add_subdirectory("{component_dir}" "${{CMAKE_BINARY_DIR}}/component")',
            f"add_executable({PROBE_TARGET} probe.cpp)",
        ]
        for target in self.probe_targets():
            lines += [
                f"if(TARGET {target})",
                f"  get_target_property(_probe_type {target} TYPE)",
                '  if(_probe_type STREQUAL "STATIC_LIBRARY" OR _probe_type STREQUAL "OBJECT_LIBRARY")',
                "    # Every object, not just the referenced ones: an undeclared dependency must fail the link.",
                f"    target_sources({PROBE_TARGET} PRIVATE $<TARGET_OBJECTS:{target}>)",
                "  endif()",
                '  if(NOT _probe_type STREQUAL "EXECUTABLE")',
                f"    target_link_libraries({PROBE_TARGET} PRIVATE {target})",
                "  endif()",
                "endif()",
            ]
        lines += [
            "function(_matter_probe_list_targets directory)",
            '  get_property(_targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)',
            "  foreach(_target IN LISTS _targets)",
            "    get_target_property(_type ${_target} TYPE)",
            f'    file(APPEND "${{CMAKE_BINARY_DIR}}/{TARGET_LIST}" "${{_target}} ${{_type}}\\n")',
            "  endforeach()",
            '  get_property(_subdirectories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)',
            "  foreach(_subdirectory IN LISTS _subdirectories)",
            '    _matter_probe_list_targets("${_subdirectory}")',
            "  endforeach()",
            "endfunction()",
            f'file(WRITE "${{CMAKE_BINARY_DIR}}/{TARGET_LIST}" "")',
            '_matter_probe_list_targets("${CMAKE_CURRENT_SOURCE_DIR}")',
        ]
        self._write(self.src / "CMakeLists.txt", "\n".join(lines) + "\n")
        body = ["// Generated: every public header through the public include path."]
        body += [f"#include <{spec}>" for spec in self.probe_headers()]
        body += ["", "int main() { return 0; }", ""]
        self._write(self.src / "probe.cpp", "\n".join(body))
        query = self.build / ".cmake" / "api" / "v1" / "query"
        query.mkdir(parents=True, exist_ok=True)
        for name in ("codemodel-v2", "cmakeFiles-v1"):
            (query / name).touch()

    @staticmethod
    def _write(path: Path, text: str) -> None:
        if not path.exists() or path.read_text(encoding="utf-8") != text:
            path.write_text(text, encoding="utf-8")

    def reset(self) -> None:
        """Delete this probe's directory, but only one this tool created."""
        if self.dir.exists():
            if not (self.dir / MARKER).is_file():
                raise RuntimeError(f"refusing to delete {self.dir}: it was not created by this tool")
            shutil.rmtree(self.dir)

    def cached_generator(self) -> str | None:
        cache = self.build / "CMakeCache.txt"
        if not cache.is_file():
            return None
        for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("CMAKE_GENERATOR:"):
                return line.split("=", 1)[1]
        return None

    def cache_value(self, key: str) -> str:
        cache = self.build / "CMakeCache.txt"
        for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith(key + ":") or line.startswith(key + "="):
                return line.split("=", 1)[1]
        return ""

    # ------------------------------------------------------------------ run

    def run(self, checks: set[str], required_capabilities: set[str]) -> ProbeOutcome:
        outcome = ProbeOutcome()
        name, variant = self.component.name, self.variant.name
        configure = Result(name, variant, "configure")
        outcome.results.append(configure)
        tc = self.toolchain
        if tc.fresh or (tc.generator and self.cached_generator() not in (None, tc.generator)):
            self.reset()
        self.write_project()
        command = [tc.cmake, "-S", str(self.src), "-B", str(self.build), f"-DCMAKE_BUILD_TYPE={tc.build_type}"]
        if tc.generator:
            command += ["-G", tc.generator]
        if tc.make_program:
            command.append(f"-DCMAKE_MAKE_PROGRAM={tc.make_program}")
        for key, value in {**self.rules.probe_cache, **self.variant.cache}.items():
            command.append(f"-D{key}={value}")
        configure.notes.append("command: " + shlex.join(command))
        code, output = _run(command)
        downstream = ["component-off", "link", "sources", "include-dirs", "configure-inputs", "unbuilt-sources"]
        if "build" in checks:
            downstream += ["build", "headers", "symbols"]
        if code != 0:
            capability = self.variant.capability
            matched = next((m.group(0) for p in self.variant.unavailable_patterns for m in [p.search(output)] if m), None)
            if capability and matched:
                reason = f"capability {capability!r} unavailable ({self.rules.capabilities[capability]}): {matched}"
                if capability in required_capabilities:
                    configure.fail("capability.unavailable", reason + "; it was required with --require-capability")
                else:
                    configure.untested(reason)
                for check in downstream:
                    outcome.results.append(Result(name, variant, check).untested(reason))
                return outcome
            configure.fail("configure.failed", f"cmake exited {code}:\n{_tail(output)}")
            for check in downstream:
                outcome.results.append(Result(name, variant, check).untested("configure failed"))
            return outcome
        try:
            self.load_file_api()
        except (OSError, ValueError, KeyError) as error:
            configure.fail("configure.file-api", f"cannot read the CMake File API reply: {error}")
            return outcome
        outcome.results += [self.check_component_off(), self.check_link(), self.check_sources(outcome.compiled),
                            self.check_include_dirs(), self.check_configure_inputs(), self.check_unbuilt_sources()]
        if "build" not in checks:
            return outcome
        build = Result(name, variant, "build")
        outcome.results.append(build)
        command = [tc.cmake, "--build", str(self.build), "--parallel", str(tc.jobs)]
        build.notes.append("command: " + shlex.join(command))
        code, output = _run(command)
        if code != 0:
            build.fail("build.failed", f"build exited {code} (an undeclared dependency fails the probe link):\n{_tail(output)}")
            outcome.results += [Result(name, variant, c).untested("build failed") for c in ("headers", "symbols")]
            return outcome
        outcome.results += [self.check_headers(), self.check_symbols()]
        return outcome

    # ------------------------------------------------------------------ File API

    def load_file_api(self) -> None:
        reply = self.build / ".cmake" / "api" / "v1" / "reply"
        indexes = sorted(reply.glob("index-*.json"))
        if not indexes:
            raise ValueError("no index file; the query was not answered")
        index = json.loads(indexes[-1].read_text(encoding="utf-8"))
        objects = {o["kind"]: o for o in index["objects"]}
        codemodel = json.loads((reply / objects["codemodel"]["jsonFile"]).read_text(encoding="utf-8"))
        configuration = codemodel["configurations"][0]
        for entry in configuration["targets"]:
            if entry["name"] in GENERATOR_TARGETS:
                continue
            data = json.loads((reply / entry["jsonFile"]).read_text(encoding="utf-8"))
            self.targets[entry["name"]] = data
            self.id_to_name[entry["id"]] = entry["name"]
            for source in data.get("sources", []):
                if "compileGroupIndex" in source:
                    path = Path(source["path"])
                    path = path if path.is_absolute() else self.src / path
                    self.compiled_paths.add(os.path.normcase(os.path.normpath(str(path))))
        files = json.loads((reply / objects["cmakeFiles"]["jsonFile"]).read_text(encoding="utf-8"))
        self.inputs = files.get("inputs", [])
        listed = self.build / TARGET_LIST
        if not listed.is_file():
            raise ValueError(f"the probe did not write {TARGET_LIST}")
        for line in listed.read_text(encoding="utf-8").splitlines():
            name, _, kind = line.strip().partition(" ")
            if name and name not in self.targets and name not in GENERATOR_TARGETS:
                # Header-only INTERFACE library: no File API entry, but its usage requirements
                # still reach the probe executable, whose compile groups and link line are audited.
                self.targets[name] = {"name": name, "type": kind, "fromTargetList": True}

    def owned(self, target: str) -> bool:
        """True for targets the probed component owns (or the probe executable)."""
        return target == PROBE_TARGET or self.owners.get(target) == self.component.name

    def owner(self, target: str) -> str | None:
        return self.component.name if target == PROBE_TARGET else self.owners.get(target)

    # ------------------------------------------------------------------ configure checks

    def check_component_off(self) -> Result:
        result = Result(self.component.name, self.variant.name, "component-off")
        present = set(self.targets) - {PROBE_TARGET}
        for target in sorted(present):
            owner = self.owners.get(target)
            if owner is None:
                result.fail("component-off.unexpected-target",
                            f"target {target} ({self.targets[target].get('type')}) is owned by no component rule")
            elif owner not in self.allowed:
                result.fail("component-off.forbidden-component",
                            f"configuring {self.component.name} alone created {target} of component {owner}")
        for target in list(self.component.targets) + list(self.variant.targets):
            if target not in present:
                result.fail("component-off.missing-target", f"required target {target} was not created")
        result.notes.append(f"targets: {', '.join(sorted(present))}")
        return result

    def check_link(self) -> Result:
        result = Result(self.component.name, self.variant.name, "link")
        artifacts: dict[str, str] = {}
        for name, data in self.targets.items():
            for artifact in data.get("artifacts", []):
                artifacts[os.path.normcase(os.path.normpath(str(self.build / artifact["path"])))] = name
        for name, data in sorted(self.targets.items()):
            owner = self.owner(name)
            if owner not in self.allowed:
                continue  # reported by component-off
            allowed = [owner] + self.rules.closure(owner)
            for dependency in data.get("dependencies", []):
                other = self.id_to_name.get(dependency["id"], dependency["id"])
                other_owner = self.owner(other)
                if other_owner is None:
                    result.fail("link.unexpected-target", f"{name} depends on {other}, which no component owns")
                elif other_owner not in allowed:
                    result.fail("link.forbidden-component", f"{name} ({owner}) depends on {other} of component {other_owner}")
            for fragment in data.get("link", {}).get("commandFragments", []):
                if fragment.get("role") != "libraries":
                    continue
                for token in _split_fragment(fragment["fragment"]):
                    self._check_library(result, name, allowed, token, artifacts)
        return result

    def _check_library(self, result: Result, target: str, allowed: list[str], token: str, artifacts: dict) -> None:
        bare = token.strip('"')
        if not bare or bare.startswith(("-Wl,", "/LIBPATH", "-L")) or bare in ("-Xlinker",):
            return
        candidate = os.path.normcase(os.path.normpath(bare if os.path.isabs(bare) else str(self.build / bare)))
        if candidate in artifacts:
            other_owner = self.owner(artifacts[candidate])
            if other_owner is None:
                result.fail("link.unexpected-target", f"{target} links {artifacts[candidate]}, which no component owns")
            elif other_owner not in allowed:
                result.fail("link.forbidden-component", f"{target} links {artifacts[candidate]} of component {other_owner}")
            return
        name = os.path.basename(bare).lower()
        system = self.rules.system_link_libraries
        if name in system or bare.lower() in system or (name.startswith("-l") and name[2:] in system):
            return
        if any(p.search(bare) for p in self.variant.allowed_link_libraries):
            return
        result.fail("link.raw-library", f"{target} links {bare}, which is neither a component target nor an allowed system library")

    def check_sources(self, compiled: dict[str, set[str]]) -> Result:
        result = Result(self.component.name, self.variant.name, "sources")
        hits: dict[str, set[str]] = {}
        for name, data in sorted(self.targets.items()):
            owner = self.owner(name)
            if owner not in self.allowed:
                continue
            component = self.rules.components[owner]
            for source in data.get("sources", []):
                kind, rel = self.classify(source["path"])
                if kind in ("build", "probe"):
                    continue
                if "compileGroupIndex" in source and kind == "repo" and name != PROBE_TARGET:
                    compiled.setdefault(rel, set()).add(owner)
                where = f"{name} ({owner}) compiles {rel or source['path']}"
                if kind == "external":
                    result.fail("source.external", f"{where}: outside the repository")
                    continue
                if under(rel, component.directory):
                    continue
                reason = audited(rel, component.legacy_sources)
                if reason:
                    hits.setdefault(reason, set()).add(rel)
                    continue
                other = self.rules.owner_of(rel)
                if other is not None:
                    result.fail("source.foreign-component", f"{where}: owned by component {other.name}; link its target instead")
                else:
                    result.fail("source.unaudited", f"{where}: a source outside {component.directory} that legacy_sources does not audit")
        note_audits(result, "legacy source", hits)
        return result

    def check_include_dirs(self) -> Result:
        result = Result(self.component.name, self.variant.name, "include-dirs")
        hits: dict[str, set[str]] = {}
        for name, data in sorted(self.targets.items()):
            owner = self.owner(name)
            if owner not in self.allowed:
                continue
            component = self.rules.components[owner]
            public = self.public_dirs(owner)
            for group in data.get("compileGroups", []):
                for include in group.get("includes", []):
                    kind, rel = self.classify(include["path"])
                    if kind in ("build", "probe"):
                        continue
                    where = f"{name} ({owner}) searches {rel or include['path']}"
                    if kind == "external":
                        if not any(p.search(rel) for p in self.variant.allowed_external_include_dirs):
                            result.fail("include-dir.external", f"{where}: outside the repository and not allowed by the variant")
                        continue
                    if name == PROBE_TARGET:
                        if not any(under(rel, d) for d in public):
                            result.fail("include-dir.public-leak",
                                        f"{where}: a consumer of {self.component.name} inherits a non-public include directory")
                        continue
                    if under(rel, component.directory) or any(under(rel, d) for d in public):
                        continue
                    reason = audited(rel, component.private_include_roots) or audited(rel, component.legacy_include_dirs)
                    if reason:
                        hits.setdefault(reason, set()).add(rel)
                        continue
                    other = self.rules.owner_of(rel)
                    if other is not None:
                        result.fail("include-dir.foreign-component", f"{where}: a directory of component {other.name}")
                    else:
                        result.fail("include-dir.unaudited", f"{where}: not audited by private_include_roots or legacy_include_dirs")
        note_audits(result, "include directory", hits)
        return result

    def check_configure_inputs(self) -> Result:
        result = Result(self.component.name, self.variant.name, "configure-inputs")
        audited_entries = self.component.configure_inputs
        dirs = [self.rules.components[c].directory for c in self.allowed]
        hits: dict[str, set[str]] = {}
        for entry in self.inputs:
            if entry.get("isCMake") or entry.get("isGenerated"):
                continue
            kind, rel = self.classify(entry["path"])
            if kind in ("build", "probe"):
                continue
            if kind == "external":
                if not any(p.search(rel) for p in self.variant.allowed_external_include_dirs):
                    result.fail("configure-input.external", f"configuration reads {rel}, outside the repository")
                continue
            if any(under(rel, d) for d in dirs):
                continue
            reason = audited(rel, audited_entries)
            if reason:
                hits.setdefault(reason, set()).add(rel)
                continue
            other = self.rules.owner_of(rel)
            if other is not None:
                result.fail("configure-input.foreign-component",
                            f"configuration reads {rel} of component {other.name}, which {self.component.name} may not use")
            else:
                result.fail("configure-input.unaudited", f"configuration reads {rel}, which configure_inputs does not audit")
        note_audits(result, "configure input", hits)
        return result

    def check_unbuilt_sources(self) -> Result:
        result = Result(self.component.name, self.variant.name, "unbuilt-sources")
        compiled = set()
        for data in self.targets.values():
            for source in data.get("sources", []):
                if "compileGroupIndex" in source:
                    kind, rel = self.classify(source["path"])
                    if kind == "repo":
                        compiled.add(rel)
        variant_only = {f"{self.component.directory}/{s}" if not under(s, self.component.directory) else s
                        for v in self.component.variants if v is not self.variant for s in v.sources}
        hits: dict[str, set[str]] = {}
        for rel in self.files:
            if not under(rel, self.component.directory) or not rel.lower().endswith(SOURCE_SUFFIXES):
                continue
            if rel in compiled or rel in variant_only or glob_match(rel, self.component.ownership_exclude):
                continue
            reason = audited(rel, self.component.unbuilt_sources)
            if reason:
                hits.setdefault(reason, set()).add(rel)
                continue
            result.fail("source.unbuilt", f"{rel} belongs to {self.component.name} but no target of the "
                                          f"{self.variant.name} configuration compiles it")
        note_audits(result, "unbuilt source", hits)
        return result

    # ------------------------------------------------------------------ build checks

    def check_headers(self) -> Result:
        result = Result(self.component.name, self.variant.name, "headers")
        generator = self.cached_generator() or ""
        if "Ninja" not in generator:
            return result.untested(f"header dependencies need the Ninja generator (configured: {generator or 'unknown'})")
        ninja = self.cache_value("CMAKE_MAKE_PROGRAM") or "ninja"
        code, output = _run([ninja, "-C", str(self.build), "-t", "deps"])
        if code != 0:
            result.fail("header.deps-unavailable", f"ninja -t deps exited {code}: {_tail(output)}")
            return result
        global_forbidden = self.rules.forbidden_external_headers
        seen: set[tuple[str, str]] = set()
        target = None
        objects = 0
        hits: dict[str, set[str]] = {}
        for line in output.splitlines():
            if not line.strip():
                continue
            if not line.startswith(" "):
                match = OBJECT_LINE.match(line)
                found = OBJECT_TARGET.search(match.group(1).replace("\\", "/")) if match else None
                target = found.group(1) if found else None
                objects += 1 if target else 0
                continue
            if target is None:
                continue
            owner = self.owner(target)
            if owner not in self.allowed or (target, line) in seen:
                continue
            seen.add((target, line))
            raw = line.strip()
            absolute = raw if os.path.isabs(raw) else str(self.build / raw)
            if os.path.normcase(os.path.normpath(absolute)) in self.compiled_paths:
                continue  # the translation unit itself; sources are audited at configure time
            kind, rel = self.classify(raw, self.build)
            if kind in ("build", "probe"):
                continue
            component = self.rules.components[owner]
            where = f"{target} ({owner}) includes {rel}"
            if kind == "external":
                if any(p.search(rel) for p in self.variant.allowed_external_headers):
                    continue
                matched = [p.pattern for p in global_forbidden + component.forbidden_external_headers if p.search(rel)]
                if matched:
                    result.fail("header.forbidden-external", f"{where} (matches {matched[0]!r})")
                continue
            public = self.public_dirs(owner)
            if target == PROBE_TARGET:
                if not any(under(rel, d) for d in public):
                    result.fail("header.public-leak", f"{where}: a consumer of {self.component.name} reaches a non-public header")
                continue
            if under(rel, component.directory) or any(under(rel, d) for d in public):
                continue
            reason = audited(rel, component.legacy_headers)
            if reason:
                hits.setdefault(reason, set()).add(rel)
                continue
            other = self.rules.owner_of(rel)
            if other is not None:
                result.fail("header.foreign-component", f"{where}: owned by component {other.name}")
            else:
                result.fail("header.unaudited", f"{where}: a header outside {component.directory} that legacy_headers does not audit")
        if objects == 0:
            result.fail("header.deps-unavailable", "ninja recorded no header dependencies; was anything compiled?")
        note_audits(result, "header", hits)
        result.notes.append(f"audited {objects} object(s)")
        return result

    def check_symbols(self) -> Result:
        result = Result(self.component.name, self.variant.name, "symbols")
        if not self.component.forbidden_symbols:
            result.notes.append("no forbidden symbol groups declared")
            return result
        compiler = os.path.basename(self.cache_value("CMAKE_CXX_COMPILER")).lower()
        msvc = compiler in ("cl.exe", "cl")
        tool = self.cache_value("CMAKE_LINKER") if msvc else self.cache_value("CMAKE_NM")
        if not tool or tool.endswith("NOTFOUND"):
            result.fail("symbol.tool-unavailable", f"no {'link.exe' if msvc else 'nm'} recorded in the CMake cache")
            return result
        own = list(self.component.targets) + list(self.component.optional_targets) + list(self.variant.targets)
        archives = 0
        for target in own:
            data = self.targets.get(target)
            if not data or data.get("type") not in ("STATIC_LIBRARY", "SHARED_LIBRARY", "MODULE_LIBRARY"):
                continue
            allowances = set(self.variant.symbol_allowances.get(target, []))
            groups = [g for g in self.component.forbidden_symbols if g not in allowances]
            for artifact in data.get("artifacts", []):
                path = Path(artifact["path"])
                path = path if path.is_absolute() else self.build / path
                if not path.suffix.lower() in (".a", ".lib", ".so", ".dll", ".dylib"):
                    continue
                command = [tool, "/dump", "/symbols", str(path)] if msvc else [tool, "-g", str(path)]
                code, output = _run(command)
                if code != 0:
                    result.fail("symbol.tool-unavailable", f"{shlex.join(command)} exited {code}: {_tail(output)}")
                    continue
                archives += 1
                symbols = _msvc_symbols(output) if msvc else _nm_symbols(output)
                for group in groups:
                    pattern = self.rules.symbol_groups[group]
                    hits = sorted({s for s in symbols if pattern.search(s)})
                    if hits:
                        sample = ", ".join(hits[:5]) + (f" and {len(hits) - 5} more" if len(hits) > 5 else "")
                        result.fail("symbol.forbidden", f"{target} references {group} symbols: {sample}")
        if archives == 0:
            result.fail("symbol.tool-unavailable", "no component archive was found to inspect")
        result.notes.append(f"inspected {archives} archive(s)")
        return result


def _split_fragment(fragment: str) -> list[str]:
    try:
        return shlex.split(fragment, posix=(os.name != "nt"))
    except ValueError:
        return fragment.split()


def _nm_symbols(output: str) -> set[str]:
    symbols = set()
    for line in output.splitlines():
        parts = line.split()
        if len(parts) >= 2 and not line.endswith(":") and len(parts[-2]) == 1:
            symbols.add(parts[-1])
    return symbols


def _msvc_symbols(output: str) -> set[str]:
    symbols = set()
    for line in output.splitlines():
        if "External" in line and "| " in line:
            symbols.add(line.split("| ", 1)[1].split(" ", 1)[0])
    return symbols
