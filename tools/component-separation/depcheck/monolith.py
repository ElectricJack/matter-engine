"""Which component sources the monolith build actually compiles.

Make and the root CMake graph both read ``cmake/manifests/*.sources``
(``path|all|windows|linux`` per line). A component source the monolith still
compiles is either listed there directly or reached through a listed
forwarding entrypoint (``#include "../../../libs/<Component>/src/x.cpp"``).
``monolith_sources`` rules entries are only honoured for paths this map
proves, so a stale manifest cannot leave the monolith silently relinking
(or silently dropping) code the rules claim it still builds.
"""

from __future__ import annotations

import os
import re
from pathlib import Path

from .rules import SOURCE_SUFFIXES

MANIFEST_DIR = "cmake/manifests"
SOURCE_INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.MULTILINE)
_CACHE: dict[Path, dict[str, str]] = {}


def monolith_entries(root: Path) -> dict[str, str]:
    """Repository-relative source -> the manifest entry that compiles it."""
    root = root.resolve()
    if root in _CACHE:
        return _CACHE[root]
    entries: dict[str, str] = {}
    for manifest in sorted((root / MANIFEST_DIR).glob("*.sources")):
        for number, raw in enumerate(manifest.read_text(encoding="utf-8").splitlines(), 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            listed = line.split("|", 1)[0].strip()
            where = f"{manifest.relative_to(root).as_posix()}:{number}"
            entries.setdefault(listed, where)
            path = root / listed
            if not path.is_file():
                continue
            for target in SOURCE_INCLUDE.findall(path.read_text(encoding="utf-8", errors="replace")):
                if not target.lower().endswith(SOURCE_SUFFIXES):
                    continue
                resolved = Path(os.path.normpath(path.parent / target))
                try:
                    rel = resolved.relative_to(root).as_posix()
                except ValueError:
                    continue
                if resolved.is_file():
                    entries.setdefault(rel, f"{where} via {listed}")
    _CACHE[root] = entries
    return entries
