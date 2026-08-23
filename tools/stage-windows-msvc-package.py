#!/usr/bin/env python3
"""Create a deterministic MatterEditor MSVC distribution tree."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
from pathlib import Path


DEPENDENCIES = [
    ("autoremesher_core", "vendored source", "third_party/autoremesher_core/LICENSE"),
    ("bc7enc", "vendored source", "third_party/bc7enc/LICENSE"),
    ("box3d", "vendored source", "third_party/box3d/LICENSE"),
    ("flecs", "vendored source", "third_party/flecs/LICENSE"),
    ("glfw", "vendored raylib source", "third_party/raylib/src/external/glfw/LICENSE.md"),
    ("dear_imgui", "vendored source", "third_party/imgui/LICENSE.txt"),
    ("imguizmo", "vendored source", "third_party/ImGuizmo/LICENSE"),
    ("ozz_animation", "vendored source", "third_party/ozz-animation/LICENSE.md"),
    ("quickjs_ng", "vendored source", "third_party/quickjs-ng/LICENSE"),
    ("vulkan_headers", "vendored headers / system loader", "third_party/Vulkan-Headers/LICENSE.md"),
]


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


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
    parser.add_argument("--revision", required=True)
    parser.add_argument("--autoremesher", choices=("true", "false"), required=True)
    args = parser.parse_args()

    root = args.root.resolve()
    editor = args.editor.resolve()
    pdb = args.pdb.resolve()
    project = root / "projects" / args.project
    engine_shared = root / "MatterEngine3" / "shared-lib"
    required = [(editor, "editor executable"), (project, "project"), (engine_shared, "engine shared library")]
    if args.config == "RelWithDebInfo":
        required.append((pdb, "RelWithDebInfo PDB"))
    for path, label in required:
        if not path.exists():
            raise SystemExit(f"{label} is missing: {path}")

    dist = args.dist.resolve()
    if dist.exists():
        shutil.rmtree(dist)
    dist.mkdir(parents=True)

    shutil.copy2(editor, dist / "editor.exe")
    if args.config == "RelWithDebInfo":
        shutil.copy2(pdb, dist / "editor.pdb")
    shutil.copytree(project, dist / "projects" / args.project, ignore=ignored)
    shutil.copytree(engine_shared, dist / "MatterEngine3" / "shared-lib", ignore=ignored)

    dependency_manifest: dict[str, dict[str, str]] = {}
    notice_parts = [
        "MatterEngine third-party notices\n",
        "This package was built from the source dependencies listed below.\n",
    ]
    for identity, source, relative_license in DEPENDENCIES:
        license_path = root / relative_license
        if not license_path.is_file():
            raise SystemExit(f"required dependency notice is missing: {license_path}")
        dependency_manifest[identity] = {
            "identity": source,
            "license": relative_license.replace("\\", "/"),
        }
        text = license_path.read_text(encoding="utf-8", errors="replace")
        notice_parts.append(f"\n===== {identity}: {relative_license} =====\n{text.rstrip()}\n")
    notices = dist / "THIRD_PARTY_NOTICES.txt"
    notices.write_text("".join(notice_parts), encoding="utf-8", newline="\n")

    file_hashes: dict[str, str] = {}
    for path in sorted((p for p in dist.rglob("*") if p.is_file()), key=lambda p: p.as_posix().lower()):
        relative = path.relative_to(dist).as_posix()
        if relative != "build_features.json":
            file_hashes[relative] = sha256(path)

    manifest = {
        "schema_version": 1,
        "project": args.project,
        "configuration": args.config,
        "source_revision": args.revision,
        "crt": "static",
        "toolchain": {
            "compiler": {"id": "MSVC", "version": args.compiler_version},
            "msvc_tools": args.msvc_tools,
            "windows_sdk": args.windows_sdk,
            "vulkan_sdk": args.vulkan_sdk,
        },
        "dependencies": dependency_manifest,
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
