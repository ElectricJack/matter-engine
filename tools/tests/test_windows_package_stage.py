#!/usr/bin/env python3
"""Safety contract tests for the Windows package staging helper."""

from __future__ import annotations

import importlib.util
import os
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
STAGER = REPOSITORY_ROOT / "tools" / "stage-windows-msvc-package.py"
SPEC = importlib.util.spec_from_file_location("matter_package_stager", STAGER)
assert SPEC and SPEC.loader
stager = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(stager)


def hydrology_digest(payload: bytes) -> int:
    digest = 1469598103934665603
    for byte in payload:
        digest ^= byte
        digest = (digest * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return digest or 1


def hydrology_string(value: str) -> bytes:
    encoded = value.encode("utf-8")
    return struct.pack("<I", len(encoded)) + encoded


def write_hydrology_artifact(
    path: Path, payload: bytes, magic: bytes = b"MHYDMSH3", version: int = 4
) -> int:
    digest = hydrology_digest(payload)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(
        magic + struct.pack("<IQQ", version, len(payload), digest) + payload
    )
    return digest


def write_ready_hydrology_network(root: Path) -> Path:
    references = [
        ("upper", "hydrology/sections/upper.mhyd", [], b"upper"),
        ("lower", "hydrology/sections/lower.mhyd", ["upper"], b"lower"),
        ("pool-one", "hydrology/handoffs/pool-one.mhyd", ["upper", "lower"], b"handoff"),
    ]
    digests: dict[str, int] = {}
    for identity, relative, _dependencies, payload in references:
        if relative.startswith("hydrology/handoffs/"):
            semantic_digest = 0xA000 + len(identity)
            write_hydrology_artifact(
                root / relative,
                payload + struct.pack("<Q", semantic_digest),
                b"MHYDHOF1",
                2,
            )
            digests[identity] = semantic_digest
        else:
            digests[identity] = write_hydrology_artifact(root / relative, payload)

    def reference(identity: str, relative: str, dependencies: list[str]) -> bytes:
        encoded = hydrology_string(identity) + hydrology_string(relative)
        encoded += struct.pack("<I", len(dependencies))
        encoded += b"".join(hydrology_string(value) for value in dependencies)
        encoded += struct.pack("<QQ", len(identity) + 1, digests[identity])
        return encoded

    payload = struct.pack("<BQQ6f", 2, 0x1234, 0x5678, 0, 0, 0, 10, 10, 10)
    payload += struct.pack("<I", 2) + hydrology_string("upper") + hydrology_string("lower")
    payload += struct.pack("<I", 2)
    payload += reference(*references[0][:3]) + reference(*references[1][:3])
    payload += struct.pack("<I", 1) + reference(*references[2][:3])
    digest = hydrology_digest(payload)
    manifest = root / "hydrology" / "network.mhyn"
    manifest.parent.mkdir(parents=True, exist_ok=True)
    manifest.write_bytes(
        b"MHYDNET1" + struct.pack("<IQQ", 1, len(payload), digest) + payload
    )
    return manifest


def make_directory_junction(link: Path, target: Path) -> None:
    """Create a real Windows directory junction without requiring admin."""
    result = subprocess.run(
        ["cmd.exe", "/d", "/c", "mklink", "/J", str(link), str(target)],
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        raise AssertionError(
            f"could not create junction {link} -> {target}: "
            f"{result.stdout}{result.stderr}"
        )


def remove_directory_alias(link: Path) -> None:
    """Remove the alias itself; never recurse through a test junction/symlink."""
    if os.path.lexists(link):
        os.rmdir(link)


class ProjectNameTests(unittest.TestCase):
    def test_accepts_repository_project_basenames(self) -> None:
        for project in (REPOSITORY_ROOT / "projects").iterdir():
            if project.is_dir():
                with self.subTest(project=project.name):
                    self.assertEqual(stager.validate_project_name(project.name), project.name)

    def test_rejects_unsafe_or_ambiguous_names(self) -> None:
        for name in (
            "",
            ".",
            "..",
            "../sentinel",
            r"..\sentinel",
            "C:escape",
            "C:\\escape",
            "/absolute",
            "two/parts",
            r"two\parts",
            "white space",
            ".hidden",
            "name;command",
            "world_demo.",
            "world_demo..",
            "CON",
            "con.txt",
            "PRN",
            "AUX.log",
            "NUL",
            "COM1",
            "com9.data",
            "LPT1",
            "lpt9.cache",
        ):
            with self.subTest(name=name), self.assertRaises(ValueError):
                stager.validate_project_name(name)


class DirectChildSafetyTests(unittest.TestCase):
    def test_creates_missing_physical_dist_root_for_first_stage(self) -> None:
        with tempfile.TemporaryDirectory(prefix="matter-stage-first-dist-") as temporary:
            repository = Path(temporary)
            project = repository / "projects" / "world_demo"
            build_root = repository / "MatterEditor" / "build"
            dist_root = build_root / "dist"
            destination = dist_root / "world_demo"
            project.mkdir(parents=True)
            build_root.mkdir(parents=True)

            created = stager.ensure_distribution_root(repository, dist_root)
            self.assertEqual(created, dist_root.resolve(strict=True))
            self.assertTrue(created.is_dir())
            staged = stager.reset_distribution_directory(
                projects_root=repository / "projects",
                project_source=project,
                dist_root=dist_root,
                destination=destination,
            )
            self.assertEqual(staged, destination.resolve(strict=True))
            self.assertEqual(list(staged.iterdir()), [])

    @unittest.skipUnless(os.name == "nt", "Windows junction contract")
    def test_rejects_reparse_build_parent_when_dist_root_is_missing(self) -> None:
        with tempfile.TemporaryDirectory(prefix="matter-stage-first-dist-junction-") as temporary:
            repository = Path(temporary) / "repository"
            matter_editor = repository / "MatterEditor"
            physical_build = Path(temporary) / "external-build"
            build_alias = matter_editor / "build"
            matter_editor.mkdir(parents=True)
            physical_build.mkdir()
            sentinel = physical_build / "must-survive.txt"
            sentinel.write_text("external", encoding="utf-8")
            make_directory_junction(build_alias, physical_build)
            try:
                with self.assertRaisesRegex(ValueError, "reparse|symlink|physical"):
                    stager.ensure_distribution_root(
                        repository, build_alias / "dist"
                    )
                self.assertFalse((physical_build / "dist").exists())
                self.assertEqual(sentinel.read_text(encoding="utf-8"), "external")
            finally:
                remove_directory_alias(build_alias)

    @unittest.skipUnless(os.name == "nt", "Windows basename alias contract")
    def test_rejects_trailing_dot_host_alias_before_rmtree(self) -> None:
        with tempfile.TemporaryDirectory(prefix="matter-stage-dot-alias-") as temporary:
            root = Path(temporary)
            projects = root / "projects"
            project = projects / "world_demo"
            dist_root = root / "dist"
            destination = dist_root / "world_demo"
            project.mkdir(parents=True)
            destination.mkdir(parents=True)
            sentinel = destination / "must-survive.txt"
            sentinel.write_text("canonical", encoding="utf-8")

            # Windows resolves the dotted spelling to the canonical directory.
            project_alias = projects / "world_demo."
            destination_alias = dist_root / "world_demo."
            self.assertEqual(project_alias.resolve(strict=True), project.resolve(strict=True))
            self.assertEqual(
                destination_alias.resolve(strict=True), destination.resolve(strict=True)
            )
            with mock.patch.object(
                stager.shutil,
                "rmtree",
                side_effect=AssertionError("rmtree reached a basename alias"),
            ):
                with self.assertRaises(ValueError):
                    stager.reset_distribution_directory(
                        projects_root=projects,
                        project_source=project_alias,
                        dist_root=dist_root,
                        destination=destination_alias,
                    )
            self.assertEqual(sentinel.read_text(encoding="utf-8"), "canonical")

    def test_rejects_outside_destination_before_deletion(self) -> None:
        with tempfile.TemporaryDirectory(prefix="matter-stage-safety-") as temporary:
            root = Path(temporary)
            projects = root / "projects"
            dist_root = root / "dist"
            project = projects / "world_demo"
            outside = root / "outside"
            project.mkdir(parents=True)
            dist_root.mkdir()
            outside.mkdir()
            sentinel = outside / "must-survive.txt"
            sentinel.write_text("sentinel", encoding="utf-8")

            with self.assertRaises(ValueError):
                stager.reset_distribution_directory(
                    projects_root=projects,
                    project_source=project,
                    dist_root=dist_root,
                    destination=outside,
                )

            self.assertEqual(sentinel.read_text(encoding="utf-8"), "sentinel")

    def test_rejects_project_alias_and_destination_alias(self) -> None:
        with tempfile.TemporaryDirectory(prefix="matter-stage-alias-") as temporary:
            root = Path(temporary)
            projects = root / "projects"
            dist_root = root / "dist"
            actual_project = projects / "world_demo"
            actual_project.mkdir(parents=True)
            dist_root.mkdir()

            for source, destination in (
                (projects / ".." / "outside-project", dist_root / "world_demo"),
                (actual_project, dist_root / "nested" / "world_demo"),
            ):
                with self.subTest(source=source, destination=destination), self.assertRaises(ValueError):
                    stager.validate_staging_paths(projects, source, dist_root, destination)

    @unittest.skipUnless(os.name == "nt", "Windows junction contract")
    def test_rejects_junction_dist_root_before_rmtree(self) -> None:
        """Replacing the dist trust root must never reach recursive deletion."""
        with tempfile.TemporaryDirectory(prefix="matter-stage-root-junction-") as temporary:
            root = Path(temporary)
            projects = root / "projects"
            project = projects / "world_demo"
            external_dist = root / "external-dist"
            external_destination = external_dist / "world_demo"
            dist_junction = root / "dist"
            project.mkdir(parents=True)
            external_destination.mkdir(parents=True)
            sentinel = external_destination / "must-survive.txt"
            sentinel.write_text("external sentinel", encoding="utf-8")
            make_directory_junction(dist_junction, external_dist)
            try:
                # The rmtree guard makes the RED safe: an implementation that
                # follows the junction fails here without touching the outside
                # sentinel. The correct implementation rejects with ValueError
                # before the guard can run.
                with mock.patch.object(
                    stager.shutil,
                    "rmtree",
                    side_effect=AssertionError("rmtree reached a junction target"),
                ):
                    with self.assertRaisesRegex(ValueError, "reparse|symlink|physical"):
                        stager.reset_distribution_directory(
                            projects_root=projects,
                            project_source=project,
                            dist_root=dist_junction,
                            destination=dist_junction / "world_demo",
                        )
                self.assertEqual(
                    sentinel.read_text(encoding="utf-8"), "external sentinel"
                )
            finally:
                remove_directory_alias(dist_junction)

    @unittest.skipUnless(os.name == "nt", "Windows junction contract")
    def test_rejects_junction_in_trust_root_ancestor(self) -> None:
        """A reparse ancestor must not disappear when the child root resolves."""
        with tempfile.TemporaryDirectory(prefix="matter-stage-ancestor-junction-") as temporary:
            root = Path(temporary)
            physical_repository = root / "physical-repository"
            projects = physical_repository / "projects"
            dist_root = physical_repository / "dist"
            project = projects / "world_demo"
            destination = dist_root / "world_demo"
            project.mkdir(parents=True)
            destination.mkdir(parents=True)
            sentinel = destination / "must-survive.txt"
            sentinel.write_text("ancestor sentinel", encoding="utf-8")
            repository_junction = root / "repository-alias"
            make_directory_junction(repository_junction, physical_repository)
            try:
                alias_projects = repository_junction / "projects"
                alias_dist = repository_junction / "dist"
                with self.assertRaisesRegex(ValueError, "reparse|symlink|physical"):
                    stager.validate_staging_paths(
                        alias_projects,
                        alias_projects / "world_demo",
                        alias_dist,
                        alias_dist / "world_demo",
                    )
                self.assertEqual(
                    sentinel.read_text(encoding="utf-8"), "ancestor sentinel"
                )
            finally:
                remove_directory_alias(repository_junction)

    @unittest.skipUnless(os.name == "nt", "Windows symlink contract")
    def test_rejects_directory_symlink_trust_root(self) -> None:
        """A real directory symlink receives the same fail-closed treatment."""
        with tempfile.TemporaryDirectory(prefix="matter-stage-root-symlink-") as temporary:
            root = Path(temporary)
            projects = root / "projects"
            project = projects / "world_demo"
            external_dist = root / "external-dist"
            destination = external_dist / "world_demo"
            dist_symlink = root / "dist-link"
            project.mkdir(parents=True)
            destination.mkdir(parents=True)
            sentinel = destination / "must-survive.txt"
            sentinel.write_text("symlink sentinel", encoding="utf-8")
            try:
                os.symlink(external_dist, dist_symlink, target_is_directory=True)
            except OSError as error:
                self.skipTest(f"directory symlinks are unavailable: {error}")
            try:
                with self.assertRaisesRegex(ValueError, "reparse|symlink|physical"):
                    stager.validate_staging_paths(
                        projects,
                        project,
                        dist_symlink,
                        dist_symlink / "world_demo",
                    )
                self.assertEqual(
                    sentinel.read_text(encoding="utf-8"), "symlink sentinel"
                )
            finally:
                remove_directory_alias(dist_symlink)

    @unittest.skipUnless(os.name == "nt", "Windows junction contract")
    def test_rechecks_original_root_immediately_before_rmtree(self) -> None:
        """A root swapped after the first check is rejected at deletion."""
        with tempfile.TemporaryDirectory(prefix="matter-stage-rmtree-swap-") as temporary:
            root = Path(temporary)
            projects = root / "projects"
            project = projects / "world_demo"
            dist_root = root / "dist"
            destination = dist_root / "world_demo"
            parked_dist = root / "parked-dist"
            external_dist = root / "external-dist"
            external_destination = external_dist / "world_demo"
            project.mkdir(parents=True)
            destination.mkdir(parents=True)
            external_destination.mkdir(parents=True)
            internal_sentinel = destination / "internal-must-survive.txt"
            external_sentinel = external_destination / "external-must-survive.txt"
            internal_sentinel.write_text("internal", encoding="utf-8")
            external_sentinel.write_text("external", encoding="utf-8")

            original_validate = stager.validate_staging_paths
            calls = 0

            def swap_after_first_validation(*args: object, **kwargs: object):
                nonlocal calls
                result = original_validate(*args, **kwargs)
                calls += 1
                if calls == 1:
                    os.rename(dist_root, parked_dist)
                    make_directory_junction(dist_root, external_dist)
                return result

            try:
                with mock.patch.object(
                    stager, "validate_staging_paths", swap_after_first_validation
                ), mock.patch.object(
                    stager.shutil,
                    "rmtree",
                    side_effect=AssertionError("rmtree reached a swapped root"),
                ):
                    with self.assertRaisesRegex(ValueError, "reparse|symlink|physical"):
                        stager.reset_distribution_directory(
                            projects_root=projects,
                            project_source=project,
                            dist_root=dist_root,
                            destination=destination,
                        )
                parked_sentinel = (
                    parked_dist / "world_demo" / "internal-must-survive.txt"
                )
                self.assertEqual(parked_sentinel.read_text(encoding="utf-8"), "internal")
                self.assertEqual(external_sentinel.read_text(encoding="utf-8"), "external")
            finally:
                remove_directory_alias(dist_root)
                if parked_dist.exists() and not dist_root.exists():
                    os.rename(parked_dist, dist_root)
    @unittest.skipUnless(os.name == "nt", "Windows junction contract")
    def test_rechecks_original_root_after_rmtree_before_recreation(self) -> None:
        """A root swapped after safe deletion cannot redirect mkdir/copy."""
        with tempfile.TemporaryDirectory(prefix="matter-stage-recreate-swap-") as temporary:
            root = Path(temporary)
            projects = root / "projects"
            project = projects / "world_demo"
            dist_root = root / "dist"
            destination = dist_root / "world_demo"
            parked_dist = root / "parked-dist"
            external_dist = root / "external-dist"
            external_destination = external_dist / "world_demo"
            project.mkdir(parents=True)
            destination.mkdir(parents=True)
            external_destination.mkdir(parents=True)
            external_sentinel = external_destination / "external-must-survive.txt"
            external_sentinel.write_text("external", encoding="utf-8")

            original_rmtree = stager.shutil.rmtree

            def safe_delete_then_swap(path: Path) -> None:
                resolved = Path(path).resolve(strict=True)
                self.assertEqual(resolved.parent, dist_root.resolve(strict=True))
                original_rmtree(resolved)
                os.rename(dist_root, parked_dist)
                make_directory_junction(dist_root, external_dist)

            try:
                with mock.patch.object(stager.shutil, "rmtree", safe_delete_then_swap):
                    with self.assertRaisesRegex(ValueError, "reparse|symlink|physical"):
                        stager.reset_distribution_directory(
                            projects_root=projects,
                            project_source=project,
                            dist_root=dist_root,
                            destination=destination,
                        )
                self.assertEqual(external_sentinel.read_text(encoding="utf-8"), "external")
            finally:
                remove_directory_alias(dist_root)
                if parked_dist.exists() and not dist_root.exists():
                    os.rename(parked_dist, dist_root)


class PhysxRuntimeStageTests(unittest.TestCase):
    def test_stages_exact_physx_gpu_runtime_and_nvidia_notices(self) -> None:
        with tempfile.TemporaryDirectory(prefix="matter-stage-physx-") as temporary:
            root = Path(temporary)
            dist = root / "dist"
            dist.mkdir()
            runtime = root / "PhysXGpu_64.dll"
            physx_license = root / "physx-license.md"
            cuda_license = root / "cuda-eula.txt"
            runtime.write_bytes(b"pinned physx gpu runtime")
            physx_license.write_text("PhysX license content", encoding="utf-8")
            cuda_license.write_text("CUDA EULA content", encoding="utf-8")

            runtime_dlls, notice_components = stager.stage_physx_runtime_bundle(
                dist,
                enabled=True,
                runtime=runtime,
                physx_license=physx_license,
                cuda_license=cuda_license,
            )

            self.assertEqual(runtime_dlls, ["PhysXGpu_64.dll"])
            self.assertEqual(
                [component[0] for component in notice_components],
                ["nvidia_physx", "nvidia_cuda"],
            )
            self.assertEqual(
                (dist / "PhysXGpu_64.dll").read_bytes(), runtime.read_bytes()
            )
            self.assertEqual(
                (dist / "licenses" / "NVIDIA_PhysX_LICENSE.md").read_text(
                    encoding="utf-8"
                ),
                physx_license.read_text(encoding="utf-8"),
            )
            self.assertEqual(
                (dist / "licenses" / "NVIDIA_CUDA_EULA.txt").read_text(
                    encoding="utf-8"
                ),
                cuda_license.read_text(encoding="utf-8"),
            )

    def test_rejects_physx_runtime_alias(self) -> None:
        with tempfile.TemporaryDirectory(
            prefix="matter-stage-physx-alias-"
        ) as temporary:
            root = Path(temporary)
            dist = root / "dist"
            dist.mkdir()
            runtime = root / "PhysXGpu_64-copy.dll"
            physx_license = root / "physx-license.md"
            cuda_license = root / "cuda-eula.txt"
            runtime.write_bytes(b"alias")
            physx_license.write_text("PhysX license content", encoding="utf-8")
            cuda_license.write_text("CUDA EULA content", encoding="utf-8")

            with self.assertRaisesRegex(ValueError, "PhysXGpu_64.dll"):
                stager.stage_physx_runtime_bundle(
                    dist,
                    enabled=True,
                    runtime=runtime,
                    physx_license=physx_license,
                    cuda_license=cuda_license,
                )

    def test_stages_one_explicit_ready_river_hydrology_network(self) -> None:
        with tempfile.TemporaryDirectory(
            prefix="matter-stage-fluid-cache-"
        ) as temporary:
            root = Path(temporary)
            dist = root / "dist"
            dist.mkdir()
            cache = root / "accepted-network"
            cache.mkdir()
            write_ready_hydrology_network(cache)

            relatives = stager.stage_hydrology_network_cache(
                dist, "world_demo", cache
            )

            prefix = "projects/world_demo/.cache/RiverHydrology/hydrology/"
            self.assertEqual(
                relatives,
                [
                    prefix + "handoffs/pool-one.mhyd",
                    prefix + "network.mhyn",
                    prefix + "sections/lower.mhyd",
                    prefix + "sections/upper.mhyd",
                ],
            )
            for relative in relatives:
                source = cache / relative.removeprefix(
                    "projects/world_demo/.cache/RiverHydrology/"
                )
                self.assertEqual((dist / relative).read_bytes(), source.read_bytes())

    def test_rejects_hydrology_network_with_missing_referenced_section(self) -> None:
        with tempfile.TemporaryDirectory(
            prefix="matter-stage-fluid-cache-missing-"
        ) as temporary:
            root = Path(temporary)
            dist = root / "dist"
            dist.mkdir()
            cache = root / "accepted-network"
            cache.mkdir()
            write_ready_hydrology_network(cache)
            (cache / "hydrology" / "sections" / "lower.mhyd").unlink()

            with self.assertRaisesRegex(ValueError, "referenced hydrology artifact"):
                stager.stage_hydrology_network_cache(dist, "world_demo", cache)


if __name__ == "__main__":
    unittest.main()
