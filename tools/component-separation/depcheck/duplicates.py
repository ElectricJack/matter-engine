"""Duplicate algorithm detection.

A component must compile shared code from where it lives (or move it), never
carry a copy. Each component source is compared with every same-language file
that belongs to a different component or to no component: identical files are
reported outright, and otherwise every run of ``window`` consecutive
significant lines (comments, blank lines, include/import/pragma lines and
lines with fewer than ``min_chars`` word characters dropped; whitespace
collapsed) that also occurs elsewhere is merged into line ranges and reported.
"""

from __future__ import annotations

import hashlib
import re
from dataclasses import dataclass

from .report import Result
from .rules import Component, Rules, glob_match, under
from .text import family, read_many, strip_comments

DIRECTIVE = re.compile(r"^\s*(?:#\s*(?:include|import|pragma)\b|import\b.*\bfrom\b|import\s*['\"]|export\s+\*\s+from\b)")
WORD = re.compile(r"\w")
SPACE = re.compile(r"\s+")


@dataclass
class Normalized:
    rel: str
    lang: str
    lines: list[tuple[int, str]]  # (source line, normalized text)
    digest: str


def normalize(rel: str, text: str, lang: str, min_chars: int) -> Normalized:
    stripped = strip_comments(text, js=(lang == "js"))
    lines = []
    for number, raw in enumerate(stripped.splitlines(), start=1):
        if DIRECTIVE.match(raw):
            continue
        line = SPACE.sub(" ", raw).strip()
        if len(WORD.findall(line)) < min_chars:
            continue
        lines.append((number, line))
    digest = hashlib.sha1("\n".join(text for _, text in lines).encode("utf-8")).hexdigest()
    return Normalized(rel, lang, lines, digest)


def _windows(item: Normalized, size: int):
    for index in range(len(item.lines) - size + 1):
        chunk = "\n".join(text for _, text in item.lines[index:index + size])
        yield index, hashlib.sha1(chunk.encode("utf-8")).digest()


def candidate_files(rules: Rules, files: list[str]) -> list[str]:
    roots = rules.duplicate_roots or [""]
    return [f for f in files
            if family(f) and any(under(f, root) for root in roots) and not glob_match(f, rules.duplicate_exclude)]


def check_duplicates(rules: Rules, files: list[str], components: list[Component]) -> list[Result]:
    """One ``duplicates`` result per implemented component in ``components``."""
    window, min_chars = rules.duplicate_window, rules.duplicate_min_chars
    candidates = candidate_files(rules, files)
    owner = {rel: (rules.owner_of(rel).name if rules.owner_of(rel) else None) for rel in candidates}
    wanted = {c.name for c in components if c.status == "implemented"}
    texts = read_many([rules.repo.abs(rel) for rel in candidates])
    normalized = {rel: normalize(rel, text, family(rel), min_chars) for rel, text in zip(candidates, texts)}

    # Index every window once; a component file then looks its own windows up.
    index: dict[bytes, list[tuple[str, int]]] = {}
    by_digest: dict[tuple[str, str], list[str]] = {}
    for rel, item in normalized.items():
        if len(item.lines) >= 3:
            by_digest.setdefault((item.lang, item.digest), []).append(rel)
        for position, key in _windows(item, window):
            index.setdefault(key, []).append((rel, position))

    allow = {pair: reason for pair, reason in rules.duplicate_allow}
    results = []
    for component in components:
        if component.name not in wanted:
            continue
        result = Result(component.name, "-", "duplicates")
        own = sorted(rel for rel in candidates if owner[rel] == component.name)
        reported: set[frozenset] = set()
        for rel in own:
            item = normalized[rel]
            identical = [other for other in by_digest.get((item.lang, item.digest), [])
                         if other != rel and owner[other] != component.name]
            for other in identical:
                pair = frozenset((rel, other))
                reported.add(pair)
                if pair in allow:
                    result.notes.append(f"allowed identical {rel} == {other}: {allow[pair]}")
                else:
                    result.fail("duplicate.identical-file",
                                f"{rel} is a copy of {other} ({_describe(owner[other])}); compile or move the original")
            spans: dict[str, list[list[int]]] = {}
            for position, key in _windows(item, window):
                for other, other_position in index.get(key, []):
                    if other == rel or owner[other] == component.name or frozenset((rel, other)) in reported:
                        continue
                    spans.setdefault(other, []).append([position, position + window - 1,
                                                        other_position, other_position + window - 1])
            for other, ranges in sorted(spans.items()):
                pair = frozenset((rel, other))
                merged = _merge(ranges)
                text = "; ".join(
                    f"{rel}:{item.lines[a][0]}-{item.lines[b][0]} ~ "
                    f"{other}:{normalized[other].lines[c][0]}-{normalized[other].lines[d][0]}"
                    for a, b, c, d in merged[:4])
                if len(merged) > 4:
                    text += f"; and {len(merged) - 4} more range(s)"
                total = sum(b - a + 1 for a, b, _, _ in merged)
                if pair in allow:
                    result.notes.append(f"allowed shared code ({total} lines) {text}: {allow[pair]}")
                else:
                    result.fail("duplicate.shared-code",
                                f"{total} significant line(s) duplicated with {_describe(owner[other])}: {text}")
        result.notes.append(f"compared {len(own)} file(s) against {len(candidates) - len(own)} candidate file(s)")
        results.append(result)
    return results


def _describe(owner: str | None) -> str:
    return f"component {owner}" if owner else "non-component code"


def _merge(ranges: list[list[int]]) -> list[list[int]]:
    """Merge overlapping (own, other) window ranges that stay diagonal."""
    merged: list[list[int]] = []
    for a, b, c, d in sorted(ranges):
        if merged:
            last = merged[-1]
            if a <= last[1] + 1 and c - a == last[2] - last[0]:
                last[1] = max(last[1], b)
                last[3] = max(last[3], d)
                continue
        merged.append([a, b, c, d])
    return merged
