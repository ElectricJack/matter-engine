"""Embed the canonical solid shaders; assets never require runtime file lookup."""
import pathlib
import struct
import sys

lines = ["#pragma once", "#include <cstdint>", "namespace matter::geometry_compute::spirv {"]
for filename in sys.argv[2:]:
    path = pathlib.Path(filename)
    raw = path.read_bytes()
    if len(raw) % 4 or raw[:4] != b"\x03\x02\x23\x07":
        raise ValueError(f"Invalid SPIR-V: {path}")
    words = struct.unpack(f"<{len(raw) // 4}I", raw)
    lines.append(f"inline constexpr uint32_t {path.name.split('.')[0]}[] = {{")
    for start in range(0, len(words), 8):
        lines.append(",".join(f"0x{w:08x}u" for w in words[start:start + 8]) + ",")
    lines.append("};")
lines.append("}")
pathlib.Path(sys.argv[1]).write_text("\n".join(lines) + "\n", encoding="utf-8")
