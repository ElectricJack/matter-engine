#!/usr/bin/env python3
"""Safety contract tests for the Windows package staging helper."""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
STAGER = REPOSITORY_ROOT / "tools" / "stage-windows-msvc-package.py"
SPEC = importlib.util.spec_from_file_location("matter_package_stager", STAGER)
assert SPEC and SPEC.loader
stager = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(stager)


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
        ):
            with self.subTest(name=name), self.assertRaises(ValueError):
                stager.validate_project_name(name)


class DirectChildSafetyTests(unittest.TestCase):
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


if __name__ == "__main__":
    unittest.main()
