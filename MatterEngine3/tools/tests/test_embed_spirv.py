"""CLI regressions for deterministic SPIR-V embedding without long argv lists."""

from __future__ import annotations

import re
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


EMBED_SCRIPT = Path(__file__).resolve().parents[1] / "embed_spirv.py"


class EmbedSpirvInputListTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="matter spirv inputs ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.output = self.root / "generated headers" / "embedded_spirv.h"

    def shader(self, name: str, word: int = 1) -> Path:
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(struct.pack("<III", 0x07230203, 0x00010600, word))
        return path

    def input_list(self, paths: list[Path]) -> Path:
        path = self.root / "SPIR V input list.txt"
        path.write_text("\n".join(str(value) for value in paths) + "\n",
                        encoding="utf-8")
        return path

    def invoke(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(EMBED_SCRIPT), str(self.output), *arguments],
            cwd=self.root, capture_output=True, text=True, encoding="utf-8",
            errors="replace", check=False,
        )

    def assert_rejected_without_replacing_header(self, input_list: Path) -> None:
        self.output.parent.mkdir(parents=True, exist_ok=True)
        self.output.write_text("previous complete header\n", encoding="utf-8")
        result = self.invoke("--input-list", str(input_list))
        self.assertNotEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertEqual("previous complete header\n",
                         self.output.read_text(encoding="utf-8"))

    def test_input_list_matches_legacy_cli_and_preserves_full_unicode_inventory(self) -> None:
        paths = [self.shader("shader binaries/zeta.comp.spv", 7),
                 self.shader("shader binaries/alpha with spaces.comp.spv", 8),
                 self.shader("shader binaries/eau \u00e9tendue.comp.spv", 9)]
        legacy = self.invoke(*(str(path) for path in paths))
        self.assertEqual(0, legacy.returncode, legacy.stdout + legacy.stderr)
        expected = self.output.read_bytes()
        self.output.unlink()

        result = self.invoke("--input-list", str(self.input_list(paths[::-1])))
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertTrue(self.output.is_file(), "successful embedding must create its header")
        self.assertEqual(expected, self.output.read_bytes())
        literals = re.findall(r'if \(name == "((?:\\x[0-9a-f]{2})+)"\)',
                              self.output.read_text(encoding="utf-8"))
        inventory = [bytes.fromhex(literal.replace("\\x", "")).decode("utf-8")
                     for literal in literals]
        self.assertEqual(sorted(path.name for path in paths), inventory)

        timestamp = self.output.stat().st_mtime_ns
        result = self.invoke("--input-list", str(self.input_list(paths)))
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertEqual(expected, self.output.read_bytes())
        self.assertEqual(timestamp, self.output.stat().st_mtime_ns,
                         "unchanged inventory must not rewrite the header")

    def test_missing_list_does_not_replace_header(self) -> None:
        self.assert_rejected_without_replacing_header(self.root / "missing list.txt")

    def test_empty_list_is_rejected(self) -> None:
        path = self.root / "empty list.txt"
        path.write_text("", encoding="utf-8")
        self.assert_rejected_without_replacing_header(path)

    def test_blank_entry_is_rejected(self) -> None:
        first = self.shader("first.comp.spv")
        second = self.shader("second.comp.spv")
        path = self.root / "blank entry.txt"
        path.write_text(f"{first}\n\n{second}\n", encoding="utf-8")
        self.assert_rejected_without_replacing_header(path)

    def test_missing_shader_is_rejected(self) -> None:
        self.assert_rejected_without_replacing_header(
            self.input_list([self.root / "missing.comp.spv"]))

    def test_duplicate_lookup_name_is_rejected_through_list(self) -> None:
        paths = [self.shader("first/shared.comp.spv"),
                 self.shader("second/shared.comp.spv")]
        self.assert_rejected_without_replacing_header(self.input_list(paths))

    def test_unaligned_spirv_is_rejected_through_list(self) -> None:
        path = self.root / "unaligned.comp.spv"
        path.write_bytes(b"\x03\x02\x23")
        self.assert_rejected_without_replacing_header(self.input_list([path]))

    def test_input_list_option_requires_a_filename(self) -> None:
        result = self.invoke("--input-list")
        self.assertNotEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
