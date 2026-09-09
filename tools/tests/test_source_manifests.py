"""Behavior tests for the compiler-neutral source-manifest gate."""

import importlib.util
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CHECKER_PATH = ROOT / "tools" / "check-source-manifests.py"
SPEC = importlib.util.spec_from_file_location("check_source_manifests", CHECKER_PATH)
assert SPEC is not None and SPEC.loader is not None
CHECKER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECKER)


class SourceManifestTests(unittest.TestCase):
    """Each test names a malformed manifest state the gate must reject."""

    def fixture(self, manifests: dict[str, str], sources: tuple[str, ...] = ()) -> Path:
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        root = Path(temp.name)
        manifest_dir = root / "cmake" / "manifests"
        manifest_dir.mkdir(parents=True)
        for name, contents in manifests.items():
            (manifest_dir / name).write_text(contents, encoding="utf-8")
        for source in sources:
            path = root / source
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("int fixture_source() { return 0; }\n", encoding="utf-8")
        return root

    def test_accepts_existing_unique_sources_for_both_platforms(self):
        root = self.fixture(
            {"engine-core.sources": "MatterEngine3/src/core.cpp|all\n",
             "editor.sources": "MatterEditor/src/win.cpp|windows\nMatterEditor/src/linux.cpp|linux\n"},
            ("MatterEngine3/src/core.cpp", "MatterEditor/src/win.cpp", "MatterEditor/src/linux.cpp"),
        )

        self.assertEqual([], CHECKER.check(root))

    def test_rejects_manifest_path_that_does_not_exist(self):
        root = self.fixture({"engine-core.sources": "MatterEngine3/src/missing.cpp|all\n"})

        self.assertIn("missing", "\n".join(CHECKER.check(root)).lower())

    def test_rejects_source_listed_twice(self):
        root = self.fixture(
            {"engine-core.sources": "MatterEngine3/src/core.cpp|all\n",
             "editor.sources": "MatterEngine3/src/core.cpp|all\n"},
            ("MatterEngine3/src/core.cpp",),
        )

        self.assertIn("duplicate", "\n".join(CHECKER.check(root)).lower())

    def test_rejects_duplicate_source_spelled_through_a_dot_path_alias(self):
        root = self.fixture(
            {"engine-core.sources": "MatterEngine3/src/core.cpp|all\n",
             "editor.sources": "MatterEngine3/src/./core.cpp|all\n"},
            ("MatterEngine3/src/core.cpp",),
        )

        self.assertIn("duplicate", "\n".join(CHECKER.check(root)).lower())

    def test_rejects_unknown_platform_tag(self):
        root = self.fixture(
            {"engine-core.sources": "MatterEngine3/src/core.cpp|macos\n"},
            ("MatterEngine3/src/core.cpp",),
        )

        self.assertIn("platform", "\n".join(CHECKER.check(root)).lower())

    def test_rejects_compilable_source_without_a_platform_classification(self):
        root = self.fixture(
            {"engine-core.sources": "MatterEngine3/src/unclassified.cpp\n"},
            ("MatterEngine3/src/unclassified.cpp",),
        )

        self.assertIn("unclassified", "\n".join(CHECKER.check(root)).lower())


if __name__ == "__main__":
    unittest.main()
