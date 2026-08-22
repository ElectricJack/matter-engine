#!/usr/bin/env python3
"""Validate the compiler-neutral source manifests used by Make and CMake."""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Iterable


PLATFORMS = {"windows", "linux", "all"}
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx"}
# This deliberately covers only first-party source roots that the migration
# owns.  Vendored trees are much broader than the direct-source inventories;
# their selected translation units are still checked for existence above.
SOURCE_SCAN_DIRS = (
    "MatterEngine3/src",
    "MatterEditor/src",
    "libs/ParticleFlowLib/src",
    "libs/MatterSurfaceLib/src",
    "libs/SpatialQueryLib/src",
    "libs/MemoryLib/src",
    "libs/MeshChartingLib/src",
    "libs/ProfileLib/src",
)
# These source files are intentionally outside the current compiler-neutral
# build graph.  Keep each exclusion named here rather than silently broadening
# a canonical compiled inventory; a newly added source in any scan root fails.
EXCLUDED_SOURCES = frozenset({
    "libs/MatterSurfaceLib/src/mesh_retopo.cpp",  # conditionally/default compiled via RETOPO/EXTRA_RETOPO_CPP, outside base manifests
    "libs/MatterSurfaceLib/src/shader_preprocessor.cpp",  # unused helper
    "libs/MatterSurfaceLib/src/voxel_imposter.cpp",  # retired implementation
    "libs/MemoryLib/src/mem_arena.c",  # standalone library API, not linked here
    "libs/MemoryLib/src/mem_array.c",  # standalone library API, not linked here
})


def _manifest_entries(manifest: Path, root: Path) -> Iterable[tuple[str, str, int]]:
    for line_number, raw_line in enumerate(manifest.read_text(encoding="utf-8").splitlines(), 1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        fields = line.split("|")
        if len(fields) != 2 or not fields[0] or not fields[1]:
            yield "", f"{manifest.relative_to(root)}:{line_number}: unclassified source requires |windows, |linux, or |all", line_number
            continue
        yield fields[0], fields[1], line_number


def check(root: Path | str) -> list[str]:
    """Return all manifest-gate violations below *root*, or an empty list."""
    root = Path(root).resolve()
    manifest_dir = root / "cmake" / "manifests"
    if not manifest_dir.is_dir():
        return [f"missing manifest directory: {manifest_dir}"]

    errors: list[str] = []
    seen: dict[str, Path] = {}
    for manifest in sorted(manifest_dir.glob("*.sources")):
        for source, platform, line_number in _manifest_entries(manifest, root):
            location = f"{manifest.relative_to(root)}:{line_number}"
            if not source:
                errors.append(platform)
                continue
            if platform not in PLATFORMS:
                errors.append(f"{location}: invalid platform tag '{platform}'")
                continue
            source_path = Path(source)
            if source_path.is_absolute():
                errors.append(f"{location}: source path must be repository-relative: {source}")
                continue
            resolved = (root / source_path).resolve()
            try:
                canonical_source = resolved.relative_to(root).as_posix()
            except ValueError:
                errors.append(f"{location}: source path escapes repository root: {source}")
                continue
            if Path(canonical_source).suffix.lower() not in SOURCE_SUFFIXES:
                errors.append(f"{location}: source is not compilable: {source}")
                continue
            if not resolved.is_file():
                errors.append(f"{location}: missing source: {source}")
                continue
            if canonical_source in seen:
                errors.append(f"{location}: duplicate source (already in {seen[canonical_source].relative_to(root)}): {source}")
                continue
            seen[canonical_source] = manifest
    for scan_dir in SOURCE_SCAN_DIRS:
        directory = root / scan_dir
        if not directory.is_dir():
            continue
        for candidate in directory.rglob("*"):
            if not candidate.is_file() or candidate.suffix.lower() not in SOURCE_SUFFIXES:
                continue
            source = candidate.relative_to(root).as_posix()
            if source not in seen and source not in EXCLUDED_SOURCES:
                errors.append(f"unclassified compilable source in {scan_dir}: {source}")
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True, help="repository root")
    args = parser.parse_args(argv)
    errors = check(args.root)
    for error in errors:
        print(f"ERROR: {error}")
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
