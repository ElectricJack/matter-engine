#!/usr/bin/env python3
"""Create a deterministic MatterEditor MSVC distribution tree."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import stat
import subprocess
from pathlib import Path


DEPENDENCIES = {
    "autoremesher_core": {
        "source": "third_party/autoremesher_core",
        "languages": ["C", "CXX"],
        "defines": [
            "_USE_MATH_DEFINES",
            "GEOGRAM_WITH_PDEL",
            "AUTOREMESHER_FORCE_SHIM",
            "STB_IMAGE_WRITE_STATIC",
        ],
        "options": ["/utf-8", "/w", "/FIcmake/MatterAutoremesherConfig.h"],
        "libraries": ["matter_autoremesher.lib"],
    },
    "bc7enc": {
        "source": "third_party/bc7enc",
        "languages": ["CXX"],
        "defines": [],
        "options": ["/utf-8", "/w"],
        "libraries": ["matter_bc7enc.lib"],
    },
    "box3d": {
        "source": "third_party/box3d",
        "languages": ["C"],
        "defines": ["B3_ENABLE_ASSERT"],
        "options": ["/utf-8", "/w"],
        "libraries": ["matter_box3d.lib"],
    },
    "flecs": {
        "source": "third_party/flecs",
        "languages": ["C"],
        "defines": [],
        "options": ["/utf-8", "/w"],
        "libraries": ["matter_flecs.lib"],
    },
    "glfw": {
        "source": "third_party/raylib/src/external/glfw",
        "languages": ["C"],
        "defines": ["_GLFW_WIN32"],
        "options": ["/utf-8", "/w"],
        "libraries": ["matter_glfw.lib"],
    },
    "dear_imgui": {
        "source": "third_party/imgui",
        "languages": ["CXX"],
        "defines": [],
        "options": ["/utf-8", "/w"],
        "libraries": ["matter_imgui.lib"],
    },
    "imguizmo": {
        "source": "third_party/ImGuizmo",
        "languages": ["CXX"],
        "defines": [],
        "options": ["/utf-8", "/w"],
        "libraries": ["matter_imguizmo.lib"],
    },
    "ozz_animation": {
        "source": "third_party/ozz-animation",
        "languages": ["CXX"],
        "defines": ["_CRT_SECURE_NO_WARNINGS"],
        "options": ["/utf-8", "/w"],
        "libraries": [
            "matter_ozz_base.lib",
            "matter_ozz_animation.lib",
            "matter_ozz_offline.lib",
        ],
    },
    "quickjs_ng": {
        "source": "third_party/quickjs-ng",
        "languages": ["C"],
        "defines": ["CONFIG_VERSION=0.10.0", "WIN32_LEAN_AND_MEAN"],
        "options": ["/utf-8", "/w"],
        "libraries": ["matter_quickjs.lib"],
    },
    "vulkan_headers": {
        "source": "third_party/Vulkan-Headers",
        "languages": ["C", "CXX"],
        "defines": [],
        "options": [],
        "libraries": [],
    },
}

NOTICE_COMPONENTS = [
    ("autoremesher_core", "third_party/autoremesher_core/LICENSE", "full"),
    ("autoremesher_geogram", "third_party/autoremesher_core/thirdparty/geogram/LICENSE", "full"),
    ("autoremesher_eigen", "third_party/autoremesher_core/thirdparty/eigen/COPYING.README", "eigen_bundle"),
    ("autoremesher_isotropicremesher", "third_party/autoremesher_core/thirdparty/isotropicremesher/LICENSE", "full"),
    ("autoremesher_zlib", "third_party/autoremesher_core/thirdparty/geogram/src/lib/geogram/third_party/zlib/LICENSE", "full"),
    ("autoremesher_rply", "third_party/autoremesher_core/thirdparty/geogram/src/lib/geogram/third_party/rply/LICENSE", "full"),
    ("autoremesher_libmeshb", "third_party/autoremesher_core/thirdparty/geogram/src/lib/geogram/third_party/libMeshb/LICENSE.txt", "full"),
    ("autoremesher_stb_image", "third_party/autoremesher_core/thirdparty/geogram/src/lib/geogram/third_party/stb_image/stb_image.h", "stb_mit"),
    ("autoremesher_stb_image_write", "third_party/autoremesher_core/thirdparty/geogram/src/lib/geogram/third_party/stb_image/stb_image_write.h", "stb_mit"),
    ("bc7enc", "third_party/bc7enc/LICENSE", "full"),
    ("box3d", "third_party/box3d/LICENSE", "full"),
    ("flecs", "third_party/flecs/LICENSE", "full"),
    ("glfw", "third_party/raylib/src/external/glfw/LICENSE.md", "full"),
    ("dear_imgui", "third_party/imgui/LICENSE.txt", "full"),
    ("imguizmo", "third_party/ImGuizmo/LICENSE", "full"),
    ("ozz_animation", "third_party/ozz-animation/LICENSE.md", "full"),
    ("quickjs_ng", "third_party/quickjs-ng/LICENSE", "full"),
    ("vulkan_headers", "third_party/Vulkan-Headers/LICENSE.md", "full"),
]

SAFE_PROJECT_NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]*$")


def validate_project_name(name: str) -> str:
    """Return a safe single-component project name or reject it."""
    if name in {"", ".", ".."} or not SAFE_PROJECT_NAME.fullmatch(name):
        raise ValueError(
            f"unsafe project name {name!r}: expected a basename matching "
            "^[A-Za-z0-9][A-Za-z0-9._-]*$"
        )
    return name


def _is_reparse_point(path: Path) -> bool:
    """Reject symlink/junction aliases at package trust boundaries."""
    try:
        metadata = path.lstat()
    except FileNotFoundError:
        return False
    attributes = getattr(metadata, "st_file_attributes", 0)
    reparse = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)
    return path.is_symlink() or bool(attributes & reparse)


def _absolute_unresolved(path: Path, label: str) -> Path:
    """Make a path absolute without resolving away symlink/reparse identity."""
    if ".." in path.parts:
        raise ValueError(f"{label} contains an ambiguous parent component: {path}")
    if not path.is_absolute():
        path = Path.cwd() / path
    return path


def _assert_no_reparse_components(path: Path, label: str) -> Path:
    """Fail closed if any existing component of the original path is an alias."""
    path = _absolute_unresolved(path, label)
    anchor = Path(path.anchor)
    current = anchor
    parts = path.parts[1:] if path.anchor else path.parts
    for part in parts:
        current = current / part
        try:
            current.lstat()
        except FileNotFoundError:
            # Descendants cannot exist once an original-path component is
            # missing. The caller separately enforces required existence.
            break
        except OSError as error:
            raise ValueError(
                f"cannot inspect {label} path component {current}: {error}"
            ) from error
        if _is_reparse_point(current):
            raise ValueError(
                f"{label} path contains a symlink or reparse point: {current}"
            )
    return path


def _assert_direct_child(
    root: Path, candidate: Path, label: str, *, must_exist: bool
) -> tuple[Path, Path]:
    original_root = _assert_no_reparse_components(root, f"{label} root")
    try:
        root = original_root.resolve(strict=True)
    except OSError as error:
        raise ValueError(f"{label} root is missing or unresolved: {original_root}") from error
    if not root.is_dir():
        raise ValueError(f"{label} root is not a physical directory: {original_root}")

    candidate = _assert_no_reparse_components(candidate, label)
    try:
        parent = candidate.parent.resolve(strict=True)
    except OSError as error:
        raise ValueError(f"{label} parent is missing or unresolved: {candidate.parent}") from error
    if parent != root:
        raise ValueError(f"{label} must be an immediate child of {root}: {candidate}")
    if candidate.exists():
        if _is_reparse_point(candidate):
            raise ValueError(f"{label} cannot be a symlink or reparse point: {candidate}")
        resolved = candidate.resolve(strict=True)
        if resolved.parent != root:
            raise ValueError(f"resolved {label} escapes {root}: {resolved}")
    elif must_exist:
        raise ValueError(f"{label} is missing: {candidate}")
    else:
        resolved = root / candidate.name
    return root, resolved


def validate_staging_paths(
    projects_root: Path,
    project_source: Path,
    dist_root: Path,
    destination: Path,
) -> tuple[Path, Path]:
    """Prove both staging paths are physical immediate children of fixed roots."""
    _, source = _assert_direct_child(
        projects_root, project_source, "project source", must_exist=True
    )
    _, dist = _assert_direct_child(
        dist_root, destination, "distribution destination", must_exist=False
    )
    if not source.is_dir():
        raise ValueError(f"project source is not a directory: {source}")
    return source, dist


def reset_distribution_directory(
    *,
    projects_root: Path,
    project_source: Path,
    dist_root: Path,
    destination: Path,
) -> Path:
    """Validate before mutation, revalidate at deletion, then recreate destination."""
    validate_staging_paths(
        projects_root, project_source, dist_root, destination
    )
    # Always repeat the original, unresolved trust-root checks immediately at
    # the mutation boundary. Do not feed a previously resolved destination
    # back into validation: that would erase the alias evidence being checked.
    _, dist = validate_staging_paths(
        projects_root, project_source, dist_root, destination
    )
    if dist.exists():
        shutil.rmtree(dist)
    # A root can be replaced between deletion and recreation. Recheck the
    # unresolved paths once more before mkdir/copy can write package content.
    _, dist = validate_staging_paths(
        projects_root, project_source, dist_root, destination
    )
    dist.mkdir()
    return dist


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def run_git(root: Path, *arguments: str, text: bool = True) -> str | bytes:
    result = subprocess.run(
        ["git", "-C", str(root), *arguments],
        check=False,
        capture_output=True,
        text=text,
    )
    if result.returncode != 0:
        error = result.stderr.strip() if text else result.stderr.decode(errors="replace").strip()
        raise SystemExit(f"git {' '.join(arguments)} failed: {error}")
    return result.stdout


def tree_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    files = sorted(
        (
            candidate
            for candidate in path.rglob("*")
            if candidate.is_file()
            and not any(part in {".git", "__pycache__", ".cache", "build"} for part in candidate.parts)
            and not candidate.name.endswith((".pyc", ".bak"))
        ),
        key=lambda candidate: candidate.relative_to(path).as_posix().lower(),
    )
    for candidate in files:
        relative = candidate.relative_to(path).as_posix().encode("utf-8")
        digest.update(relative)
        digest.update(b"\0")
        digest.update(bytes.fromhex(sha256(candidate)))
    return digest.hexdigest()


def read_notice(path: Path, mode: str) -> str:
    text = path.read_text(encoding="utf-8", errors="replace")
    if mode == "full":
        return text.rstrip()
    if mode == "eigen_bundle":
        sections = [(path.name, text.rstrip())]
        for name in (
            "COPYING.MPL2",
            "COPYING.BSD",
            "COPYING.LGPL",
            "COPYING.GPL",
            "COPYING.MINPACK",
        ):
            license_path = path.parent / name
            if not license_path.is_file():
                raise SystemExit(f"required Eigen license is missing: {license_path}")
            sections.append(
                (
                    name,
                    license_path.read_text(encoding="utf-8", errors="replace").rstrip(),
                )
            )
        return "\n\n".join(f"--- Eigen {name} ---\n{body}" for name, body in sections)
    if mode == "stb_mit":
        start_marker = "ALTERNATIVE A - MIT License"
        end_marker = "ALTERNATIVE B - Public Domain"
        start = text.rfind(start_marker)
        end = text.find(end_marker, start)
        if start < 0 or end < 0:
            raise SystemExit(f"could not extract embedded stb MIT license from {path}")
        return text[start:end].rstrip()
    raise SystemExit(f"unknown notice extraction mode {mode!r} for {path}")


def parse_dependency_libraries(values: list[str]) -> dict[str, list[Path]]:
    result: dict[str, list[Path]] = {name: [] for name in DEPENDENCIES}
    for value in values:
        if "=" not in value:
            raise SystemExit(f"dependency library must use NAME=PATH: {value}")
        name, raw_path = value.split("=", 1)
        if name not in DEPENDENCIES:
            raise SystemExit(f"unknown dependency library identity: {name}")
        result[name].append(Path(raw_path).resolve())
    for name, specification in DEPENDENCIES.items():
        actual_names = [path.name for path in result[name]]
        if actual_names != specification["libraries"]:
            raise SystemExit(
                f"dependency {name} library mismatch: expected {specification['libraries']}, got {actual_names}"
            )
        for path in result[name]:
            if not path.is_file():
                raise SystemExit(f"dependency library is missing: {path}")
    return result


def ignored(_directory: str, names: list[str]) -> set[str]:
    return {
        name
        for name in names
        if name in {".cache", "backup", "__pycache__"}
        or name.endswith(".bak")
        or name.endswith(".pyc")
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--editor", required=True, type=Path)
    parser.add_argument("--pdb", required=True, type=Path)
    parser.add_argument("--dist", required=True, type=Path)
    parser.add_argument("--project", required=True)
    parser.add_argument("--config", required=True)
    parser.add_argument("--compiler-version", required=True)
    parser.add_argument("--msvc-tools", required=True)
    parser.add_argument("--windows-sdk", required=True)
    parser.add_argument("--vulkan-sdk", required=True)
    parser.add_argument("--autoremesher", choices=("true", "false"), required=True)
    parser.add_argument("--dependency-library", action="append", default=[])
    args = parser.parse_args()

    try:
        original_root = _assert_no_reparse_components(args.root, "repository root")
        root = original_root.resolve(strict=True)
    except (OSError, ValueError) as error:
        raise SystemExit(f"unsafe repository root: {error}") from error
    try:
        project_name = validate_project_name(args.project)
    except ValueError as error:
        raise SystemExit(str(error)) from error
    editor = args.editor.resolve()
    pdb = args.pdb.resolve()
    # Keep the caller's original absolute spelling through every trust-root
    # check. Resolving here would erase a repository/projects/build/dist alias.
    projects_root = original_root / "projects"
    project_source = projects_root / project_name
    dist_root = original_root / "MatterEditor" / "build" / "dist"
    try:
        requested_dist = _absolute_unresolved(
            args.dist, "distribution destination"
        )
    except ValueError as error:
        raise SystemExit(f"unsafe package staging path: {error}") from error
    if requested_dist.name != project_name:
        raise SystemExit(
            f"distribution destination name must match project {project_name!r}: {requested_dist}"
        )
    engine_shared = root / "MatterEngine3" / "shared-lib"
    try:
        project, _ = validate_staging_paths(
            projects_root, project_source, dist_root, requested_dist
        )
    except (OSError, ValueError) as error:
        raise SystemExit(f"unsafe package staging path: {error}") from error
    required = [(editor, "editor executable"), (project, "project"), (engine_shared, "engine shared library")]
    if args.config == "RelWithDebInfo":
        required.append((pdb, "RelWithDebInfo PDB"))
    for path, label in required:
        if not path.exists():
            raise SystemExit(f"{label} is missing: {path}")
    dependency_libraries = parse_dependency_libraries(args.dependency_library)

    revision = str(run_git(root, "rev-parse", "HEAD")).strip()
    if not re.fullmatch(r"[0-9a-f]{40}", revision):
        raise SystemExit(f"git returned an invalid source revision: {revision!r}")
    tracked_status = str(run_git(root, "status", "--porcelain", "--untracked-files=no"))
    tracked_dirty = bool(tracked_status.strip())
    diff = bytes(run_git(root, "diff", "--binary", "--no-ext-diff", "HEAD", "--", text=False))
    diff_sha256 = hashlib.sha256(diff).hexdigest()

    try:
        dist = reset_distribution_directory(
            projects_root=projects_root,
            project_source=project_source,
            dist_root=dist_root,
            destination=requested_dist,
        )
        # Repeat the unresolved component checks immediately before copying
        # package content; never trust the previously resolved return values.
        project, dist = validate_staging_paths(
            projects_root, project_source, dist_root, requested_dist
        )
    except (OSError, ValueError) as error:
        raise SystemExit(f"unsafe package staging path: {error}") from error

    shutil.copy2(editor, dist / "editor.exe")
    if args.config == "RelWithDebInfo":
        shutil.copy2(pdb, dist / "editor.pdb")
    shutil.copytree(project, dist / "projects" / project_name, ignore=ignored)
    shutil.copytree(engine_shared, dist / "MatterEngine3" / "shared-lib", ignore=ignored)

    dependency_manifest: dict[str, dict[str, object]] = {}
    for identity, specification in DEPENDENCIES.items():
        source_path = root / str(specification["source"])
        if not source_path.is_dir():
            raise SystemExit(f"dependency source is missing: {source_path}")
        git_tree = str(run_git(root, "rev-parse", f"HEAD:{specification['source']}")).strip()
        if not re.fullmatch(r"[0-9a-f]{40}", git_tree):
            raise SystemExit(f"dependency {identity} has invalid git tree identity: {git_tree!r}")
        artifacts = [
            {"name": path.name, "size": path.stat().st_size, "sha256": sha256(path)}
            for path in dependency_libraries[identity]
        ]
        dependency_manifest[identity] = {
            "source": {
                "path": specification["source"],
                "git_tree": git_tree,
                "tree_sha256": tree_sha256(source_path),
            },
            "build": {
                "languages": specification["languages"],
                "runtime": "static" if artifacts else "none",
                "options": specification["options"],
                "defines": specification["defines"],
            },
            "artifacts": artifacts,
        }

    notice_parts = [
        "MatterEngine third-party notices\n",
        "This package was built from the source dependencies listed below.\n",
    ]
    notice_labels: list[str] = []
    for identity, relative_license, mode in NOTICE_COMPONENTS:
        license_path = root / relative_license
        if not license_path.is_file():
            raise SystemExit(f"required dependency notice is missing: {license_path}")
        notice_labels.append(identity)
        text = read_notice(license_path, mode)
        notice_parts.append(f"\n===== {identity}: {relative_license} =====\n{text}\n")
    notices = dist / "THIRD_PARTY_NOTICES.txt"
    notices.write_text("".join(notice_parts), encoding="utf-8", newline="\n")

    file_hashes: dict[str, str] = {}
    for path in sorted((p for p in dist.rglob("*") if p.is_file()), key=lambda p: p.as_posix().lower()):
        relative = path.relative_to(dist).as_posix()
        if relative != "build_features.json":
            file_hashes[relative] = sha256(path)

    manifest = {
        "schema_version": 2,
        "project": project_name,
        "configuration": args.config,
        "source": {
            "revision": revision,
            "tracked_dirty": tracked_dirty,
            "diff_sha256": diff_sha256,
        },
        "crt": {
            "linkage": "static",
            "cmake": "MultiThreaded$<$<CONFIG:Debug>:Debug>",
        },
        "toolchain": {
            "compiler": {"id": "MSVC", "version": args.compiler_version},
            "msvc_tools": args.msvc_tools,
            "windows_sdk": args.windows_sdk,
            "vulkan_sdk": args.vulkan_sdk,
        },
        "dependencies": dependency_manifest,
        "notices": notice_labels,
        "features": {
            "autoremesher": args.autoremesher == "true",
            "streamline": False,
            "physx": False,
            "cuda": False,
            "vulkan_renderer": True,
        },
        "runtime_dlls": [],
        "files": file_hashes,
    }
    (dist / "build_features.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8", newline="\n"
    )
    print(f"MSVC dist staged: {dist} ({len(file_hashes)} hashed files, 0 staged runtime DLLs)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
