"""Rules loading, strict validation and repository path helpers.

Rules are strict on purpose: an unknown key, an unknown component reference,
an undeclared capability or a missing reason on an audited exception is a
usage error (exit 2), never a silently ignored setting.
"""

from __future__ import annotations

import fnmatch
import json
import os
import re
import subprocess
from dataclasses import dataclass, field
from pathlib import Path

SCHEMA = "matter.component-dependency-rules.v1"
STATUSES = ("implemented", "scaffold", "absent")
KINDS = ("native", "javascript", "browser", "adapter")
SOURCE_SUFFIXES = (".c", ".cc", ".cpp", ".cxx")
HEADER_SUFFIXES = (".h", ".hh", ".hpp", ".hxx", ".inl")
JS_SUFFIXES = (".mjs", ".js", ".cjs", ".mts")
# Directory names that never hold first-party component code.
SKIPPED_DIR_NAMES = {".git", ".aq", "build", "node_modules", "__pycache__", ".pytest_cache", ".cmake"}


class RulesError(Exception):
    """Malformed rules or command line: reported with exit code 2."""


def _norm(text: str) -> str:
    return os.path.normcase(text)


class Repo:
    """Path arithmetic relative to one repository root (POSIX relative keys)."""

    def __init__(self, root: Path):
        self.root = root.resolve()

    def abs(self, rel: str) -> Path:
        return (self.root / rel).resolve()

    def rel(self, path: Path | str) -> str | None:
        """Repository-relative POSIX path, or None when outside the root."""
        candidate = Path(path)
        if not candidate.is_absolute():
            return None
        resolved = Path(os.path.normpath(str(candidate)))
        try:
            return resolved.relative_to(self.root).as_posix()
        except ValueError:
            pass
        # Windows File API paths differ only in case or separator style.
        a, b = _norm(str(resolved)), _norm(str(self.root))
        if a == b:
            return ""
        if a.startswith(b.rstrip("\\/") + os.sep):
            return Path(str(resolved)[len(str(self.root)) + 1:]).as_posix()
        return None


def under(rel: str, prefix: str) -> bool:
    """True when repository-relative ``rel`` equals or lives below ``prefix``."""
    rel, prefix = _norm(rel).replace("\\", "/"), _norm(prefix).replace("\\", "/").rstrip("/")
    return prefix == "" or rel == prefix or rel.startswith(prefix + "/")


def glob_match(rel: str, patterns: list[str]) -> bool:
    return any(fnmatch.fnmatchcase(rel, pattern) for pattern in patterns)


def _compile(patterns: list[str], where: str) -> list[re.Pattern]:
    compiled = []
    for pattern in patterns:
        try:
            compiled.append(re.compile(pattern))
        except re.error as error:
            raise RulesError(f"{where}: invalid regular expression {pattern!r}: {error}") from error
    return compiled


def _expect(obj: dict, allowed: set[str], required: set[str], where: str) -> None:
    if not isinstance(obj, dict):
        raise RulesError(f"{where}: expected an object")
    unknown = set(obj) - allowed
    if unknown:
        raise RulesError(f"{where}: unknown key(s) {sorted(unknown)}")
    missing = required - set(obj)
    if missing:
        raise RulesError(f"{where}: missing required key(s) {sorted(missing)}")


def _strings(value, where: str) -> list[str]:
    if not isinstance(value, list) or not all(isinstance(item, str) and item for item in value):
        raise RulesError(f"{where}: expected a list of non-empty strings")
    return list(value)


def _audited(value, where: str, key: str = "path") -> list[tuple[str, str]]:
    """``[{path|paths, reason}]`` -> [(path, reason)]; every exception needs a reason."""
    if not isinstance(value, list):
        raise RulesError(f"{where}: expected a list")
    entries = []
    for index, item in enumerate(value):
        here = f"{where}[{index}]"
        _expect(item, {key, key + "s", "reason"}, {"reason"}, here)
        reason = item["reason"]
        if not isinstance(reason, str) or len(reason.strip()) < 8:
            raise RulesError(f"{here}: audited exception needs a meaningful reason")
        paths = []
        if key in item:
            paths.append(item[key])
        if key + "s" in item:
            paths.extend(_strings(item[key + "s"], here + "." + key + "s"))
        if not paths or not all(isinstance(p, str) and p for p in paths):
            raise RulesError(f"{here}: needs '{key}' or '{key}s'")
        entries.extend((path.rstrip("/"), reason) for path in paths)
    return entries


@dataclass
class Variant:
    name: str
    cache: dict[str, str] = field(default_factory=dict)
    targets: list[str] = field(default_factory=list)
    public_headers: list[str] = field(default_factory=list)
    sources: list[str] = field(default_factory=list)
    capability: str | None = None
    unavailable_patterns: list[re.Pattern] = field(default_factory=list)
    allowed_external_headers: list[re.Pattern] = field(default_factory=list)
    allowed_external_include_dirs: list[re.Pattern] = field(default_factory=list)
    allowed_link_libraries: list[re.Pattern] = field(default_factory=list)
    symbol_allowances: dict[str, list[str]] = field(default_factory=dict)


@dataclass
class Component:
    name: str
    status: str
    kind: str
    directory: str
    reason: str = ""
    targets: list[str] = field(default_factory=list)
    optional_targets: list[str] = field(default_factory=list)
    vendored_targets: list[str] = field(default_factory=list)
    public_include_dirs: list[str] = field(default_factory=list)
    allowed_components: list[str] = field(default_factory=list)
    public_include_roots: list[str] = field(default_factory=list)
    private_include_roots: list[tuple[str, str]] = field(default_factory=list)
    legacy_sources: list[tuple[str, str]] = field(default_factory=list)
    legacy_include_dirs: list[tuple[str, str]] = field(default_factory=list)
    legacy_headers: list[tuple[str, str]] = field(default_factory=list)
    unbuilt_sources: list[tuple[str, str]] = field(default_factory=list)
    monolith_sources: list[tuple[str, str]] = field(default_factory=list)
    configure_inputs: list[tuple[str, str]] = field(default_factory=list)
    ownership_exclude: list[str] = field(default_factory=list)
    forbidden_public_includes: list[re.Pattern] = field(default_factory=list)
    forbidden_external_headers: list[re.Pattern] = field(default_factory=list)
    forbidden_symbols: list[str] = field(default_factory=list)
    header_overrides: dict[str, list[re.Pattern]] = field(default_factory=dict)
    js_include: list[str] = field(default_factory=list)
    js_exclude: list[str] = field(default_factory=list)
    js_forbidden_specifiers: list[re.Pattern] = field(default_factory=list)
    js_allowed_bare_specifiers: list[str] = field(default_factory=list)
    variants: list[Variant] = field(default_factory=list)

    def all_targets(self) -> list[str]:
        names = list(self.targets) + list(self.optional_targets) + list(self.vendored_targets)
        for variant in self.variants:
            names.extend(variant.targets)
        return list(dict.fromkeys(names))


@dataclass
class Rules:
    path: Path
    repo: Repo
    ownership_manifest: str | None
    capabilities: dict[str, str]
    symbol_groups: dict[str, re.Pattern]
    system_link_libraries: set[str]
    forbidden_external_headers: list[re.Pattern]
    probe_cache: dict[str, str]
    duplicate_roots: list[str]
    duplicate_exclude: list[str]
    duplicate_window: int
    duplicate_min_chars: int
    duplicate_allow: list[tuple[frozenset[str], str]]
    components: dict[str, Component]

    def owner_of(self, rel: str) -> Component | None:
        """Component whose directory is the longest prefix of ``rel``."""
        best = None
        for component in self.components.values():
            if under(rel, component.directory) and (best is None or len(component.directory) > len(best.directory)):
                best = component
        return best

    def closure(self, name: str) -> list[str]:
        """Allowed components reachable from ``name`` (excluding itself)."""
        seen: list[str] = []
        stack = list(self.components[name].allowed_components)
        while stack:
            current = stack.pop()
            if current in seen or current == name:
                continue
            seen.append(current)
            stack.extend(self.components[current].allowed_components)
        return seen

    def target_owner(self) -> dict[str, str]:
        owners: dict[str, str] = {}
        for component in self.components.values():
            for target in component.all_targets():
                owners[target] = component.name
        return owners


def _variant(raw: dict, where: str, capabilities: dict[str, str]) -> Variant:
    _expect(raw, {"name", "cache", "targets", "public_headers", "sources", "requires_capability",
                  "unavailable_patterns", "allowed_external_headers", "allowed_external_include_dirs",
                  "allowed_link_libraries", "symbol_allowances", "description"}, {"name"}, where)
    capability = raw.get("requires_capability")
    if capability is not None and capability not in capabilities:
        raise RulesError(f"{where}: requires undeclared capability {capability!r}")
    if capability is not None and not raw.get("unavailable_patterns"):
        raise RulesError(f"{where}: a capability-gated variant must say how unavailability is detected")
    cache = raw.get("cache", {})
    if not isinstance(cache, dict) or not all(isinstance(v, str) for v in cache.values()):
        raise RulesError(f"{where}.cache: expected string values")
    allowances = raw.get("symbol_allowances", {})
    if not isinstance(allowances, dict):
        raise RulesError(f"{where}.symbol_allowances: expected an object")
    return Variant(
        name=raw["name"], cache=dict(cache),
        targets=_strings(raw.get("targets", []), where + ".targets"),
        public_headers=_strings(raw.get("public_headers", []), where + ".public_headers"),
        sources=_strings(raw.get("sources", []), where + ".sources"),
        capability=capability,
        unavailable_patterns=_compile(_strings(raw.get("unavailable_patterns", []), where), where),
        allowed_external_headers=_compile(_strings(raw.get("allowed_external_headers", []), where), where),
        allowed_external_include_dirs=_compile(_strings(raw.get("allowed_external_include_dirs", []), where), where),
        allowed_link_libraries=_compile(_strings(raw.get("allowed_link_libraries", []), where), where),
        symbol_allowances={k: _strings(v, f"{where}.symbol_allowances.{k}") for k, v in allowances.items()},
    )


COMPONENT_KEYS = {
    "name", "status", "kind", "directory", "reason", "targets", "optional_targets", "vendored_targets",
    "public_include_dirs", "allowed_components", "public_include_roots", "private_include_roots",
    "legacy_sources", "legacy_include_dirs", "legacy_headers", "unbuilt_sources", "monolith_sources",
    "configure_inputs", "ownership_exclude",
    "forbidden_public_includes", "forbidden_external_headers", "forbidden_symbols", "header_overrides",
    "js", "variants", "description",
}


def _component(raw: dict, where: str, capabilities: dict[str, str], groups: dict) -> Component:
    _expect(raw, COMPONENT_KEYS, {"name", "status", "kind", "directory"}, where)
    if raw["status"] not in STATUSES:
        raise RulesError(f"{where}: status must be one of {STATUSES}")
    if raw["kind"] not in KINDS:
        raise RulesError(f"{where}: kind must be one of {KINDS}")
    if raw["status"] != "implemented" and len(str(raw.get("reason", "")).strip()) < 8:
        raise RulesError(f"{where}: {raw['status']} components must state why they are untested (reason)")
    component = Component(name=raw["name"], status=raw["status"], kind=raw["kind"],
                          directory=raw["directory"].rstrip("/"), reason=raw.get("reason", ""))
    component.targets = _strings(raw.get("targets", []), where + ".targets")
    component.optional_targets = _strings(raw.get("optional_targets", []), where + ".optional_targets")
    component.vendored_targets = _strings(raw.get("vendored_targets", []), where + ".vendored_targets")
    component.public_include_dirs = [p.rstrip("/") for p in _strings(raw.get("public_include_dirs", []), where)]
    component.allowed_components = _strings(raw.get("allowed_components", []), where + ".allowed_components")
    component.public_include_roots = [p.rstrip("/") for p in _strings(raw.get("public_include_roots", []), where)]
    component.private_include_roots = _audited(raw.get("private_include_roots", []), where + ".private_include_roots")
    component.legacy_sources = _audited(raw.get("legacy_sources", []), where + ".legacy_sources")
    component.legacy_include_dirs = _audited(raw.get("legacy_include_dirs", []), where + ".legacy_include_dirs")
    component.legacy_headers = _audited(raw.get("legacy_headers", []), where + ".legacy_headers")
    component.unbuilt_sources = _audited(raw.get("unbuilt_sources", []), where + ".unbuilt_sources")
    component.monolith_sources = _audited(raw.get("monolith_sources", []), where + ".monolith_sources")
    component.configure_inputs = _audited(raw.get("configure_inputs", []), where + ".configure_inputs")
    component.ownership_exclude = _strings(raw.get("ownership_exclude", []), where + ".ownership_exclude")
    component.forbidden_public_includes = _compile(_strings(raw.get("forbidden_public_includes", []), where), where)
    component.forbidden_external_headers = _compile(_strings(raw.get("forbidden_external_headers", []), where), where)
    component.forbidden_symbols = _strings(raw.get("forbidden_symbols", []), where + ".forbidden_symbols")
    for group in component.forbidden_symbols:
        if group not in groups:
            raise RulesError(f"{where}: unknown symbol group {group!r}")
    overrides = raw.get("header_overrides", [])
    if not isinstance(overrides, list):
        raise RulesError(f"{where}.header_overrides: expected a list")
    for index, item in enumerate(overrides):
        here = f"{where}.header_overrides[{index}]"
        _expect(item, {"path", "allowed_includes", "reason"}, {"path", "allowed_includes", "reason"}, here)
        component.header_overrides[item["path"]] = _compile(_strings(item["allowed_includes"], here), here)
    js = raw.get("js")
    if js is not None:
        _expect(js, {"include", "exclude", "forbidden_specifiers", "allowed_bare_specifiers"}, {"include"}, where + ".js")
        component.js_include = _strings(js["include"], where + ".js.include")
        component.js_exclude = _strings(js.get("exclude", []), where + ".js.exclude")
        component.js_forbidden_specifiers = _compile(_strings(js.get("forbidden_specifiers", []), where), where)
        component.js_allowed_bare_specifiers = _strings(js.get("allowed_bare_specifiers", []), where)
    raw_variants = raw.get("variants") or [{"name": "default"}]
    component.variants = [_variant(v, f"{where}.variants[{i}]", capabilities) for i, v in enumerate(raw_variants)]
    names = [v.name for v in component.variants]
    if len(set(names)) != len(names):
        raise RulesError(f"{where}: duplicate variant names {names}")
    for variant in component.variants:
        for target, allowed in variant.symbol_allowances.items():
            for group in allowed:
                if group not in groups:
                    raise RulesError(f"{where}.variants.{variant.name}: unknown symbol group {group!r}")
    if component.status == "implemented" and component.kind == "native" and not component.targets:
        raise RulesError(f"{where}: an implemented native component needs at least one target")
    return component


def load_rules(path: Path, root: Path) -> Rules:
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise RulesError(f"cannot read rules {path}: {error}") from error
    _expect(raw, {"schema", "description", "ownership_manifest", "capabilities", "symbol_groups",
                  "system_link_libraries", "forbidden_external_headers", "probe_cache", "duplicates",
                  "components"}, {"schema", "components"}, "rules")
    if raw["schema"] != SCHEMA:
        raise RulesError(f"rules: unsupported schema {raw['schema']!r} (expected {SCHEMA})")
    capabilities = raw.get("capabilities", {})
    if not isinstance(capabilities, dict):
        raise RulesError("rules.capabilities: expected an object")
    groups = {}
    for name, pattern in raw.get("symbol_groups", {}).items():
        groups[name] = _compile([pattern], f"rules.symbol_groups.{name}")[0]
    duplicates = raw.get("duplicates", {})
    _expect(duplicates, {"roots", "exclude", "window", "min_chars", "allow"}, set(), "rules.duplicates")
    allow = []
    for index, item in enumerate(duplicates.get("allow", [])):
        here = f"rules.duplicates.allow[{index}]"
        _expect(item, {"paths", "reason"}, {"paths", "reason"}, here)
        paths = _strings(item["paths"], here)
        if len(paths) != 2 or len(item["reason"].strip()) < 8:
            raise RulesError(f"{here}: needs exactly two paths and a reason")
        allow.append((frozenset(paths), item["reason"]))
    probe_cache = raw.get("probe_cache", {})
    if not isinstance(probe_cache, dict) or not all(isinstance(v, str) for v in probe_cache.values()):
        raise RulesError("rules.probe_cache: expected string values")
    components: dict[str, Component] = {}
    for index, item in enumerate(raw["components"]):
        component = _component(item, f"rules.components[{index}]", capabilities, groups)
        if component.name in components:
            raise RulesError(f"rules: duplicate component {component.name!r}")
        components[component.name] = component
    owners: dict[str, str] = {}
    for component in components.values():
        for other in component.allowed_components:
            if other not in components:
                raise RulesError(f"rules: component {component.name!r} allows unknown component {other!r}")
            if components[other].status != "implemented" and component.status == "implemented":
                raise RulesError(f"rules: implemented {component.name!r} cannot depend on {components[other].status} {other!r}")
        for target in component.all_targets():
            if target in owners and owners[target] != component.name:
                raise RulesError(f"rules: target {target!r} claimed by {owners[target]!r} and {component.name!r}")
            owners[target] = component.name
    rules = Rules(
        path=path, repo=Repo(root), ownership_manifest=raw.get("ownership_manifest"),
        capabilities=dict(capabilities), symbol_groups=groups,
        system_link_libraries={s.lower() for s in _strings(raw.get("system_link_libraries", []), "rules.system_link_libraries")},
        forbidden_external_headers=_compile(_strings(raw.get("forbidden_external_headers", []), "rules"), "rules"),
        probe_cache=dict(probe_cache),
        duplicate_roots=_strings(duplicates.get("roots", []), "rules.duplicates.roots"),
        duplicate_exclude=_strings(duplicates.get("exclude", []), "rules.duplicates.exclude"),
        duplicate_window=int(duplicates.get("window", 8)), duplicate_min_chars=int(duplicates.get("min_chars", 12)),
        duplicate_allow=allow, components=components)
    for component in components.values():
        rules.closure(component.name)  # cycles terminate; nothing else to validate
    return rules


def corpus(repo: Repo) -> list[str]:
    """Tracked plus untracked-but-not-ignored files; filesystem walk outside git.

    The git listing is taken as is, without a stat per file: on a mounted
    Windows drive from WSL each stat costs ~10 ms, which made the listing alone
    take two minutes. A tracked file deleted in the working tree reads as empty.
    """
    try:
        completed = subprocess.run(["git", "-C", str(repo.root), "ls-files", "-co", "--exclude-standard", "-z"],
                                   capture_output=True, check=True)
        top = subprocess.run(["git", "-C", str(repo.root), "rev-parse", "--show-toplevel"],
                             capture_output=True, text=True, check=True).stdout.strip()
        if Path(top).resolve() == repo.root:
            return sorted({f for f in completed.stdout.decode("utf-8", "replace").split("\0") if f})
    except (OSError, subprocess.CalledProcessError):
        pass
    found = []
    for directory, subdirs, names in os.walk(repo.root):
        subdirs[:] = [d for d in subdirs if d not in SKIPPED_DIR_NAMES]
        for name in names:
            found.append((Path(directory) / name).relative_to(repo.root).as_posix())
    return sorted(found)
