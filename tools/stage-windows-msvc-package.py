#!/usr/bin/env python3
"""Create a deterministic MatterEditor MSVC distribution tree."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import shutil
import stat
import struct
import subprocess
from pathlib import Path, PurePosixPath


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
WINDOWS_DEVICE_STEM = re.compile(
    r"^(?:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])$", re.IGNORECASE
)


def validate_project_name(name: str) -> str:
    """Return a safe single-component project name or reject it."""
    device_stem = name.split(".", 1)[0]
    if (
        name in {"", ".", ".."}
        or name.endswith(".")
        or WINDOWS_DEVICE_STEM.fullmatch(device_stem)
        or not SAFE_PROJECT_NAME.fullmatch(name)
    ):
        raise ValueError(
            f"unsafe project name {name!r}: expected a basename matching "
            "^[A-Za-z0-9][A-Za-z0-9._-]*$ without Windows trailing-dot "
            "or DOS-device aliases"
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
    root: Path,
    candidate: Path,
    label: str,
    *,
    must_exist: bool,
    expected_name: str,
) -> tuple[Path, Path]:
    expected_name = validate_project_name(expected_name)
    original_root = _assert_no_reparse_components(root, f"{label} root")
    try:
        root = original_root.resolve(strict=True)
    except OSError as error:
        raise ValueError(f"{label} root is missing or unresolved: {original_root}") from error
    if not root.is_dir():
        raise ValueError(f"{label} root is not a physical directory: {original_root}")

    candidate = _assert_no_reparse_components(candidate, label)
    if candidate.name.casefold() != expected_name.casefold():
        raise ValueError(
            f"{label} authored basename mismatch: {candidate.name!r} != "
            f"{expected_name!r}"
        )
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
        if resolved.name.casefold() != expected_name.casefold():
            raise ValueError(
                f"resolved {label} basename mismatch: {resolved.name!r} != "
                f"{expected_name!r}"
            )
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
    project_name = validate_project_name(project_source.name)
    destination_name = validate_project_name(destination.name)
    if project_name.casefold() != destination_name.casefold():
        raise ValueError(
            "project source and distribution destination basenames differ: "
            f"{project_name!r} != {destination_name!r}"
        )
    _, source = _assert_direct_child(
        projects_root,
        project_source,
        "project source",
        must_exist=True,
        expected_name=project_name,
    )
    _, dist = _assert_direct_child(
        dist_root,
        destination,
        "distribution destination",
        must_exist=False,
        expected_name=project_name,
    )
    if not source.is_dir():
        raise ValueError(f"project source is not a directory: {source}")
    return source, dist


def ensure_distribution_root(repository_root: Path, dist_root: Path) -> Path:
    """Create only the exact physical MatterEditor/build/dist trust root."""
    original_repository = _assert_no_reparse_components(
        repository_root, "repository root"
    )
    try:
        resolved_repository = original_repository.resolve(strict=True)
    except OSError as error:
        raise ValueError(
            f"repository root is missing or unresolved: {original_repository}"
        ) from error
    if not resolved_repository.is_dir():
        raise ValueError(f"repository root is not a directory: {original_repository}")

    expected_build = original_repository / "MatterEditor" / "build"
    expected_dist = expected_build / "dist"
    original_dist = _absolute_unresolved(dist_root, "distribution root")
    if str(original_dist).casefold() != str(expected_dist).casefold():
        raise ValueError(
            "distribution root is not the exact repository build root: "
            f"{original_dist}"
        )

    _assert_no_reparse_components(expected_build, "distribution build parent")
    try:
        resolved_build = expected_build.resolve(strict=True)
    except OSError as error:
        raise ValueError(
            f"distribution build parent is missing or unresolved: {expected_build}"
        ) from error
    if not resolved_build.is_dir():
        raise ValueError(
            f"distribution build parent is not a physical directory: {expected_build}"
        )
    if resolved_build.parent.parent != resolved_repository:
        raise ValueError(
            f"distribution build parent escaped the repository: {resolved_build}"
        )

    _assert_no_reparse_components(original_dist, "distribution root")
    if original_dist.exists():
        resolved_dist = original_dist.resolve(strict=True)
        if (
            not resolved_dist.is_dir()
            or resolved_dist.parent != resolved_build
            or resolved_dist.name.casefold() != "dist"
        ):
            raise ValueError(
                "distribution root is not the physical build/dist directory: "
                f"{original_dist}"
            )
        return resolved_dist

    # Recheck the original trust chain immediately before creating the one
    # permitted missing directory. Parents are deliberately not synthesized.
    _assert_no_reparse_components(original_repository, "repository root")
    _assert_no_reparse_components(expected_build, "distribution build parent")
    if expected_build.resolve(strict=True) != resolved_build:
        raise ValueError("distribution build parent changed before creation")
    original_dist.mkdir()

    # Prove the created root is still the exact physical child before any
    # destination validation, recursive deletion, or package copying can run.
    _assert_no_reparse_components(original_dist, "distribution root")
    resolved_dist = original_dist.resolve(strict=True)
    if (
        not resolved_dist.is_dir()
        or resolved_dist.parent != resolved_build
        or resolved_dist.name.casefold() != "dist"
    ):
        raise ValueError(
            "created distribution root failed physical-child validation: "
            f"{original_dist}"
        )
    return resolved_dist


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


def stage_physx_runtime_bundle(
    dist: Path,
    *,
    enabled: bool,
    runtime: Path | None,
    physx_license: Path | None,
    cuda_license: Path | None,
) -> tuple[list[str], list[tuple[str, Path]]]:
    """Stage the single pinned dynamic PhysX module and NVIDIA notices."""
    supplied = (runtime, physx_license, cuda_license)
    if not enabled:
        if any(path is not None for path in supplied):
            raise ValueError("disabled PhysX packaging cannot accept runtime aliases")
        return [], []
    if any(path is None for path in supplied):
        raise ValueError(
            "enabled PhysX packaging requires its runtime and both NVIDIA notices"
        )

    assert runtime is not None
    assert physx_license is not None
    assert cuda_license is not None
    if runtime.name != "PhysXGpu_64.dll":
        raise ValueError(
            f"PhysX runtime must use the exact pinned name PhysXGpu_64.dll: {runtime}"
        )
    for path, label in (
        (runtime, "PhysX GPU runtime"),
        (physx_license, "NVIDIA PhysX license"),
        (cuda_license, "NVIDIA CUDA EULA"),
    ):
        if not path.is_file():
            raise ValueError(f"{label} is missing: {path}")

    shutil.copy2(runtime, dist / runtime.name)
    licenses = dist / "licenses"
    licenses.mkdir()
    staged_physx_license = licenses / "NVIDIA_PhysX_LICENSE.md"
    staged_cuda_license = licenses / "NVIDIA_CUDA_EULA.txt"
    shutil.copy2(physx_license, staged_physx_license)
    shutil.copy2(cuda_license, staged_cuda_license)
    return [runtime.name], [
        ("nvidia_physx", staged_physx_license),
        ("nvidia_cuda", staged_cuda_license),
    ]


def _hydrology_digest(payload: bytes) -> int:
    digest = 1469598103934665603
    for byte in payload:
        digest ^= byte
        digest = (digest * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return digest or 1


class _HydrologyReader:
    def __init__(self, payload: bytes) -> None:
        self.payload = payload
        self.offset = 0

    def take(self, format_string: str) -> tuple[object, ...]:
        size = struct.calcsize(format_string)
        if self.offset + size > len(self.payload):
            raise ValueError("hydrology network manifest is truncated")
        values = struct.unpack_from(format_string, self.payload, self.offset)
        self.offset += size
        return values

    def string(self) -> str:
        (size,) = self.take("<I")
        if not isinstance(size, int) or size > 4096 or self.offset + size > len(self.payload):
            raise ValueError("hydrology network manifest contains an invalid string")
        raw = self.payload[self.offset : self.offset + size]
        self.offset += size
        try:
            return raw.decode("utf-8")
        except UnicodeDecodeError as error:
            raise ValueError("hydrology network manifest contains non-UTF-8 text") from error


def _read_hydrology_artifact_digest(path: Path) -> int:
    data = path.read_bytes()
    artifact_headers = {
        b"MHYDMSH3": 4,
        b"MHYDHOF1": 2,
    }
    if len(data) < 28 or data[:8] not in artifact_headers:
        raise ValueError(f"referenced hydrology artifact has invalid magic: {path}")
    version, payload_size, expected_digest = struct.unpack_from("<IQQ", data, 8)
    payload = data[28:]
    if version != artifact_headers[data[:8]] or payload_size != len(payload):
        raise ValueError(f"referenced hydrology artifact has an invalid header: {path}")
    if _hydrology_digest(payload) != expected_digest:
        raise ValueError(f"referenced hydrology artifact has an invalid digest: {path}")
    if data[:8] == b"MHYDHOF1":
        if len(payload) < 8:
            raise ValueError(f"referenced hydrology handoff has no semantic digest: {path}")
        (semantic_digest,) = struct.unpack_from("<Q", payload, len(payload) - 8)
        if semantic_digest == 0:
            raise ValueError(f"referenced hydrology handoff has an invalid semantic digest: {path}")
        return semantic_digest
    return expected_digest


def _cache_relative_hydrology_path(value: str, category: str) -> PurePosixPath:
    path = PurePosixPath(value)
    if (
        not value
        or "\\" in value
        or path.is_absolute()
        or any(part in ("", ".", "..") for part in path.parts)
        or path.as_posix() != value
        or len(path.parts) != 3
        or path.parts[0] != "hydrology"
        or path.parts[1] != category
        or path.suffix.lower() != ".mhyd"
    ):
        raise ValueError(f"hydrology network manifest contains unsafe {category} path: {value!r}")
    return path


def _read_ready_hydrology_network(cache: Path) -> tuple[Path, list[PurePosixPath]]:
    manifests = sorted(cache.glob("hydrology/*.mhyn"))
    if len(manifests) != 1:
        raise ValueError("accepted hydrology cache must contain exactly one network .mhyn manifest")
    manifest = manifests[0]
    data = manifest.read_bytes()
    if len(data) < 28 or data[:8] != b"MHYDNET1":
        raise ValueError("hydrology network manifest has invalid magic")
    version, payload_size, expected_digest = struct.unpack_from("<IQQ", data, 8)
    payload = data[28:]
    if version != 1 or payload_size != len(payload) or _hydrology_digest(payload) != expected_digest:
        raise ValueError("hydrology network manifest has an invalid header or digest")

    reader = _HydrologyReader(payload)
    state, network_key, terrain_revision, *bounds = reader.take("<BQQ6f")
    if (
        state != 2
        or network_key == 0
        or terrain_revision == 0
        or not all(math.isfinite(float(value)) for value in bounds)
        or not all(float(bounds[index]) < float(bounds[index + 3]) for index in range(3))
    ):
        raise ValueError("hydrology network manifest is not a finite Ready network")

    (order_count,) = reader.take("<I")
    if not isinstance(order_count, int) or order_count == 0 or order_count > 4096:
        raise ValueError("hydrology network manifest has an invalid section order")
    order = [reader.string() for _ in range(order_count)]
    if len(set(order)) != len(order):
        raise ValueError("hydrology network manifest has duplicate section order entries")

    def references(category: str) -> list[tuple[str, PurePosixPath, list[str], int]]:
        (count,) = reader.take("<I")
        if not isinstance(count, int) or count > 4096 or (category == "sections" and count == 0):
            raise ValueError(f"hydrology network manifest has an invalid {category} count")
        result: list[tuple[str, PurePosixPath, list[str], int]] = []
        identities: set[str] = set()
        for _ in range(count):
            identity = reader.string()
            relative = _cache_relative_hydrology_path(reader.string(), category)
            (dependency_count,) = reader.take("<I")
            if not isinstance(dependency_count, int) or dependency_count > 4096:
                raise ValueError("hydrology network manifest has too many dependencies")
            dependencies = [reader.string() for _ in range(dependency_count)]
            semantic_key, payload_digest = reader.take("<QQ")
            if (
                not identity
                or identity in identities
                or semantic_key == 0
                or payload_digest == 0
                or len(set(dependencies)) != len(dependencies)
            ):
                raise ValueError(f"hydrology network manifest contains an invalid {category} reference")
            identities.add(identity)
            result.append((identity, relative, dependencies, int(payload_digest)))
        return result

    sections = references("sections")
    handoffs = references("handoffs")
    if reader.offset != len(payload):
        raise ValueError("hydrology network manifest has trailing payload bytes")

    section_ids = {identity for identity, *_rest in sections}
    if set(order) != section_ids or len(order) != len(sections):
        raise ValueError("hydrology network section order does not match its references")
    positions = {identity: index for index, identity in enumerate(order)}
    for identity, _relative, dependencies, _digest in sections:
        if any(dependency not in positions or positions[dependency] >= positions[identity]
               for dependency in dependencies):
            raise ValueError("hydrology network section dependencies are not topological")
    for _identity, _relative, dependencies, _digest in handoffs:
        if any(dependency not in section_ids for dependency in dependencies):
            raise ValueError("hydrology network handoff references an unknown section")

    referenced: list[PurePosixPath] = []
    for _identity, relative, _dependencies, payload_digest in sections + handoffs:
        native_relative = Path(*relative.parts)
        artifact = cache / native_relative
        if not artifact.is_file():
            raise ValueError(f"referenced hydrology artifact is missing: {relative.as_posix()}")
        if _read_hydrology_artifact_digest(artifact) != payload_digest:
            raise ValueError(f"referenced hydrology artifact digest does not match: {relative.as_posix()}")
        referenced.append(relative)
    if len({path.as_posix() for path in referenced}) != len(referenced):
        raise ValueError("hydrology network manifest references an artifact more than once")

    expected_files = {manifest.name, *(path.as_posix() for path in referenced)}
    expected_files.remove(manifest.name)
    expected_files.add(manifest.relative_to(cache).as_posix())
    actual_files = {
        path.relative_to(cache).as_posix()
        for path in (cache / "hydrology").rglob("*")
        if path.is_file()
    }
    if actual_files != expected_files:
        raise ValueError("accepted hydrology cache contains unreferenced or missing files")
    return manifest, sorted(referenced, key=lambda value: value.as_posix())


def stage_hydrology_network_cache(
    dist: Path, project_name: str, cache: Path
) -> list[str]:
    """Validate and stage one complete, explicitly accepted Ready network."""
    validate_project_name(project_name)
    if not cache.is_dir():
        raise ValueError(f"accepted hydrology cache must be a directory: {cache}")
    manifest, referenced = _read_ready_hydrology_network(cache)
    destination_root = (
        dist / "projects" / project_name / ".cache" / "RiverHydrology"
    )
    destination_root.mkdir(parents=True, exist_ok=True)
    sources = [manifest, *(cache / Path(*relative.parts) for relative in referenced)]
    staged: list[str] = []
    for source in sources:
        relative = source.relative_to(cache)
        destination = destination_root / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
        staged.append(destination.relative_to(dist).as_posix())
    return sorted(staged)


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
    parser.add_argument("--physx", choices=("true", "false"), required=True)
    parser.add_argument("--physx-runtime", type=Path)
    parser.add_argument("--physx-license", type=Path)
    parser.add_argument("--cuda-license", type=Path)
    parser.add_argument("--hydrology-cache", type=Path)
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
        ensure_distribution_root(original_root, dist_root)
        project, _ = validate_staging_paths(
            projects_root, project_source, dist_root, requested_dist
        )
    except (OSError, ValueError) as error:
        raise SystemExit(f"unsafe package staging path: {error}") from error
    required = [(editor, "editor executable"), (project, "project"), (engine_shared, "engine shared library")]
    if args.config == "RelWithDebInfo":
        required.append((pdb, "RelWithDebInfo PDB"))
    if args.physx == "true":
        if args.hydrology_cache is None:
            raise SystemExit(
                "enabled PhysX packaging requires one accepted RiverHydrology network cache"
            )
        required.append((args.hydrology_cache, "accepted RiverHydrology network cache"))
    elif args.hydrology_cache is not None:
        raise SystemExit("disabled PhysX packaging cannot stage a hydrology network cache")
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

    try:
        runtime_dlls, nvidia_notices = stage_physx_runtime_bundle(
            dist,
            enabled=args.physx == "true",
            runtime=args.physx_runtime,
            physx_license=args.physx_license,
            cuda_license=args.cuda_license,
        )
    except ValueError as error:
        raise SystemExit(f"invalid PhysX package inputs: {error}") from error
    assert args.hydrology_cache is not None or args.physx == "false"
    if args.hydrology_cache is not None:
        try:
            stage_hydrology_network_cache(dist, project_name, args.hydrology_cache)
        except ValueError as error:
            raise SystemExit(f"invalid hydrology network cache: {error}") from error

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
    for identity, license_path in nvidia_notices:
        notice_labels.append(identity)
        text = read_notice(license_path, "full")
        relative_license = license_path.relative_to(dist).as_posix()
        notice_parts.append(
            f"\n===== {identity}: {relative_license} =====\n{text}\n"
        )
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
            "physx": args.physx == "true",
            "cuda": args.physx == "true",
            "vulkan_renderer": True,
        },
        "runtime_dlls": runtime_dlls,
        "files": file_hashes,
    }
    (dist / "build_features.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8", newline="\n"
    )
    print(
        f"MSVC dist staged: {dist} "
        f"({len(file_hashes)} hashed files, {len(runtime_dlls)} staged runtime DLLs)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
