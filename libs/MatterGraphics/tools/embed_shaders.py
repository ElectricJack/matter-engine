"""Embed the actual production SPIR-V; no runtime path/source resolver."""
import pathlib
import struct
import sys

output = pathlib.Path(sys.argv[1])
lines = ["#pragma once", "#include <cstdint>", "namespace matter::graphics::spirv {"]
for source in sys.argv[2:]:
    path = pathlib.Path(source)
    data = path.read_bytes()
    if len(data) % 4 or data[:4] != b"\x03\x02\x23\x07":
        raise ValueError(f"Invalid SPIR-V: {path}")
    name = path.name.replace(".", "_")
    words = struct.unpack(f"<{len(data) // 4}I", data)
    lines.append(f"inline constexpr uint32_t {name}[] = {{")
    lines.extend(",".join(f"0x{v:08x}u" for v in words[i:i+8]) + "," for i in range(0, len(words), 8))
    lines.append("};")
lines.append("}")
output.write_text("\n".join(lines) + "\n")
