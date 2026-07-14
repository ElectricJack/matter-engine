#!/usr/bin/env python3

import importlib.util
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).parents[1] / "tools" / "embed_spirv.py"
SPEC = importlib.util.spec_from_file_location("embed_spirv", SCRIPT)
assert SPEC and SPEC.loader
embed_spirv = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(embed_spirv)


class EmbedSpirvTests(unittest.TestCase):
    def test_cpp_string_literal_escapes_adversarial_bytes(self):
        escaped = embed_spirv.cpp_string_literal('quote" slash\\ line\n ctrl\x01 z')
        self.assertNotIn("\n", escaped)
        self.assertEqual(
            escaped,
            '"\\x71\\x75\\x6f\\x74\\x65\\x22\\x20\\x73\\x6c'
            '\\x61\\x73\\x68\\x5c\\x20\\x6c\\x69\\x6e\\x65\\x0a'
            '\\x20\\x63\\x74\\x72\\x6c\\x01\\x20\\x7a"',
        )

    def test_symbols_have_safe_prefix_and_collisions_are_stable(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            paths = []
            for name in ["_Upper.spv", "__double.spv", "a-b.spv", "a_b.spv"]:
                path = root / name
                path.write_bytes(struct.pack("<I", 0x07230203))
                paths.append(path)
            header = embed_spirv.render(list(reversed(paths)))

        symbols = [
            line.split()[3].split("[")[0]
            for line in header.splitlines()
            if line.startswith("inline constexpr uint32_t")
        ]
        self.assertTrue(all(symbol.startswith("matter_spirv_") for symbol in symbols))
        self.assertTrue(all("__" not in symbol for symbol in symbols))
        self.assertIn("matter_spirv_a_b_spv", symbols)
        self.assertIn("matter_spirv_a_b_spv_2", symbols)

    def test_generated_header_with_quote_filename_compiles(self):
        compiler = "C:/msys64/ucrt64/bin/g++.exe"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            shader = root / 'quote"name.spv'
            shader.write_bytes(struct.pack("<I", 0x07230203))
            header = root / "embedded.h"
            header.write_text(embed_spirv.render([shader]), encoding="utf-8")
            source = root / "check.cpp"
            source.write_text(
                '#include "embedded.h"\n'
                'int main() { return matter::find_spirv("quote\\\"name.spv").word_count == 1 ? 0 : 1; }\n',
                encoding="utf-8",
            )
            windows_source = subprocess.check_output(
                ["cygpath", "-w", str(source)], text=True
            ).strip()
            windows_root = subprocess.check_output(
                ["cygpath", "-w", str(root)], text=True
            ).strip()
            compiled = subprocess.run(
                [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-fsyntax-only", windows_source],
                cwd=windows_root,
                capture_output=True,
                text=True,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)

    def test_rejects_non_word_sized_input(self):
        with tempfile.TemporaryDirectory() as directory:
            shader = Path(directory) / "bad.spv"
            shader.write_bytes(b"abc")
            with self.assertRaisesRegex(ValueError, "not divisible by 4"):
                embed_spirv.render([shader])


if __name__ == "__main__":
    unittest.main()
