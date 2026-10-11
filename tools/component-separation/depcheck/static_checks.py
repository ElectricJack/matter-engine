"""Checks that need no compiler: module ownership, the public include closure
of native components and the import surface of JavaScript components."""

from __future__ import annotations

import json
import posixpath

from .report import Result
from .rules import Component, Rules, SOURCE_SUFFIXES, HEADER_SUFFIXES, glob_match, under
from .text import STANDARD_HEADERS, c_includes, js_specifiers, read_text

# Vendored and retired trees are not candidates for a first-party header owner.
NON_FIRST_PARTY = ("third_party", "Prototypes")


class Corpus:
    """The repository's file list plus the lookups the static checks share."""

    def __init__(self, rules: Rules, files: list[str]):
        self.rules = rules
        self.files = files
        self.file_set = set(files)
        self.headers_by_name: dict[str, list[str]] = {}
        for rel in files:
            if rel.lower().endswith(HEADER_SUFFIXES) and not any(under(rel, p) for p in NON_FIRST_PARTY):
                self.headers_by_name.setdefault(posixpath.basename(rel), []).append(rel)

    def under_dir(self, directory: str) -> list[str]:
        return [f for f in self.files if under(f, directory)]

    def suffix_matches(self, spec: str) -> list[str]:
        spec = spec.replace("\\", "/").lstrip("./")
        return [rel for rel in self.headers_by_name.get(posixpath.basename(spec), [])
                if rel == spec or rel.endswith("/" + spec)]


# --------------------------------------------------------------------------- ownership

def check_repository_ownership(rules: Rules, selected: list[str]) -> Result:
    """Directory overlaps and ownership-manifest entries no rule covers."""
    result = Result("repository", "-", "ownership")
    names = sorted(rules.components)
    for i, a in enumerate(names):
        for b in names[i + 1:]:
            da, db = rules.components[a].directory, rules.components[b].directory
            if under(da, db) or under(db, da):
                result.fail("ownership.overlap", f"{a} ({da}) and {b} ({db}) claim overlapping directories")
    manifest = _load_manifest(rules)
    if manifest is None:
        result.notes.append("no ownership manifest configured")
        return result
    if isinstance(manifest, str):
        result.fail("ownership.manifest-mismatch", manifest)
        return result
    by_directory = {c.directory: c for c in rules.components.values()}
    for entry in _manifest_entries(manifest):
        if entry.get("directory") not in by_directory:
            result.fail("ownership.manifest-unmapped",
                        f"manifest module {entry.get('target') or entry.get('directory')} "
                        f"({entry.get('directory')}) has no dependency rule")
    return result


def _load_manifest(rules: Rules):
    if not rules.ownership_manifest:
        return None
    path = rules.repo.abs(rules.ownership_manifest)
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        return f"cannot read ownership manifest {rules.ownership_manifest}: {error}"


def _manifest_entries(manifest: dict) -> list[dict]:
    entries = []
    if isinstance(manifest.get("contracts"), dict):
        entries.append(manifest["contracts"])
    entries.extend(e for e in manifest.get("modules", []) if isinstance(e, dict))
    entries.extend(e for e in manifest.get("non_native", []) if isinstance(e, dict))
    return entries


def check_component_ownership(rules: Rules, corpus: Corpus, component: Component) -> Result:
    result = Result(component.name, "-", "ownership")
    directory = rules.repo.abs(component.directory)
    exists = directory.is_dir()
    if component.status == "absent":
        if exists:
            result.fail("ownership.absent-exists",
                        f"{component.directory} now exists; declare its status and rules so it is checked")
        return result
    if not exists:
        result.fail("ownership.missing-directory", f"{component.directory} does not exist")
        return result
    build_file = {"native": "CMakeLists.txt", "javascript": "package.json"}.get(component.kind)
    if build_file and not (directory / build_file).is_file():
        result.fail("ownership.missing-build-file", f"{component.directory}/{build_file} is missing")
    if component.status == "scaffold":
        for rel in corpus.under_dir(component.directory):
            if rel.lower().endswith(SOURCE_SUFFIXES) and not glob_match(rel, component.ownership_exclude):
                result.fail("ownership.scaffold-has-sources",
                            f"{rel} implements code in a scaffold; mark the component implemented so it is audited")
    manifest = _load_manifest(rules)
    if isinstance(manifest, dict):
        owners = rules.target_owner()
        for entry in _manifest_entries(manifest):
            if entry.get("directory") != component.directory:
                continue
            target = entry.get("target")
            if target and component.kind == "native" and target not in component.all_targets():
                result.fail("ownership.manifest-mismatch",
                            f"manifest target {target} is not a target of rule {component.name}")
            declared = set()
            for dependency in entry.get("dependencies", []):
                owner = owners.get(dependency)
                if owner is None:
                    result.fail("ownership.manifest-mismatch",
                                f"manifest dependency {dependency} of {component.name} names no rule target")
                else:
                    declared.add(owner)
            if "dependencies" in entry and declared != set(component.allowed_components):
                result.fail("ownership.manifest-mismatch",
                            f"{component.name} allows {sorted(component.allowed_components)} but the manifest "
                            f"declares {sorted(declared)}")
    return result


# --------------------------------------------------------------------------- public includes

def variant_public_headers(rules: Rules, component: Component) -> dict[str, list]:
    """Repository-relative variant-only public header -> variants that ship it."""
    mapping: dict[str, list] = {}
    for variant in component.variants:
        for header in variant.public_headers:
            mapping.setdefault(_component_rel(component, header), []).append(variant)
    return mapping


def _component_rel(component: Component, path: str) -> str:
    return path if under(path, component.directory) else posixpath.join(component.directory, path)


def own_public_headers(corpus: Corpus, component: Component) -> list[str]:
    headers = []
    for directory in component.public_include_dirs:
        headers.extend(f for f in corpus.under_dir(directory) if f.lower().endswith(HEADER_SUFFIXES))
    return sorted(set(headers))


def check_public_includes(rules: Rules, corpus: Corpus, component: Component) -> Result:
    result = Result(component.name, "-", "public-includes")
    allowed = rules.closure(component.name)
    allowed_dirs = [d for name in allowed for d in rules.components[name].public_include_dirs]
    own_dirs = list(component.public_include_dirs)
    search = own_dirs + allowed_dirs + list(component.public_include_roots)
    variant_headers = variant_public_headers(rules, component)
    roots = own_public_headers(corpus, component)
    if not roots:
        result.fail("public-include.unresolved", f"{component.name} has no public headers under {own_dirs}")
        return result
    queue = [(header, header) for header in roots]
    seen = set()
    while queue:
        header, origin = queue.pop()
        if header in seen:
            continue
        seen.add(header)
        variants = variant_headers.get(origin, [])
        external_allowances = [p for v in variants for p in v.allowed_external_headers]
        overrides = component.header_overrides.get(header, [])
        text = read_text(rules.repo.abs(header))
        for line, delimiter, spec in c_includes(text):
            closing = ">" if delimiter == "<" else '"'
            where = f"{header}:{line} includes {delimiter}{spec}{closing}"
            if any(p.search(spec) for p in overrides) or any(p.search(spec) for p in external_allowances):
                continue
            resolved = None
            if delimiter == '"':
                candidate = posixpath.normpath(posixpath.join(posixpath.dirname(header), spec))
                if candidate in corpus.file_set:
                    resolved = candidate
            if resolved is None:
                for directory in search:
                    candidate = posixpath.normpath(posixpath.join(directory, spec))
                    if candidate in corpus.file_set:
                        resolved = candidate
                        break
            forbidden = [p.pattern for p in component.forbidden_public_includes if p.search(spec)]
            if forbidden:
                result.fail("public-include.forbidden", f"{where} (matches {forbidden[0]!r}; via {origin})")
                continue
            if resolved is not None:
                if any(under(resolved, d) for d in own_dirs) or any(under(resolved, d) for d in component.public_include_roots):
                    queue.append((resolved, origin))
                elif any(under(resolved, d) for d in allowed_dirs):
                    pass  # an allowed component's own check covers its closure
                else:
                    owner = rules.owner_of(resolved)
                    if owner is not None and owner.name != component.name:
                        result.fail("public-include.foreign-component",
                                    f"{where} -> {resolved} belongs to {owner.name}, which {component.name} may not use")
                    else:
                        result.fail("public-include.outside-public-roots",
                                    f"{where} -> {resolved} is not under a public include directory; consumers cannot reach it")
                continue
            if spec in STANDARD_HEADERS:
                continue
            matches = corpus.suffix_matches(spec)
            if not matches:
                continue  # a system or toolchain header; forbidden ones were matched above
            foreign = sorted({o.name for o in (rules.owner_of(m) for m in matches) if o and o.name != component.name})
            if foreign:
                result.fail("public-include.foreign-component",
                            f"{where} resolves only to {', '.join(matches[:3])} owned by {', '.join(foreign)}")
            else:
                result.fail("public-include.unresolved",
                            f"{where} resolves only to repository header(s) {', '.join(matches[:3])} outside the public include roots")
    result.notes.append(f"scanned {len(seen)} header(s) from {len(roots)} public header(s)")
    return result


# --------------------------------------------------------------------------- JavaScript imports

def check_js_imports(rules: Rules, corpus: Corpus, component: Component) -> Result:
    result = Result(component.name, "-", "js-imports")
    files = [f for f in corpus.under_dir(component.directory)
             if glob_match(posixpath.relpath(f, component.directory), component.js_include)
             and not glob_match(posixpath.relpath(f, component.directory), component.js_exclude)]
    if not files:
        result.fail("js-import.escapes-package", f"no JavaScript sources matched {component.js_include}")
        return result
    for rel in sorted(files):
        for line, spec in js_specifiers(read_text(rules.repo.abs(rel))):
            where = f"{rel}:{line} imports '{spec}'"
            forbidden = [p.pattern for p in component.js_forbidden_specifiers if p.search(spec)]
            if forbidden:
                result.fail("js-import.forbidden", f"{where} (matches {forbidden[0]!r})")
            elif spec.startswith(("./", "../")) or spec in (".", ".."):
                target = posixpath.normpath(posixpath.join(posixpath.dirname(rel), spec))
                if not under(target, component.directory):
                    result.fail("js-import.escapes-package", f"{where} -> {target} is outside {component.directory}")
            elif spec.startswith("/") or ":" in spec:
                result.fail("js-import.forbidden", f"{where} is an absolute or scheme specifier")
            elif spec not in component.js_allowed_bare_specifiers:
                result.fail("js-import.undeclared",
                            f"{where} is a bare module that is neither standard language nor an injected host interface")
    result.notes.append(f"scanned {len(files)} JavaScript file(s)")
    return result


def untested(component: Component, check: str) -> Result:
    reason = component.reason or f"component is {component.status}"
    return Result(component.name, "-", check).untested(f"{component.status}: {reason}")
