#!/usr/bin/env python3
import pathlib
import sys

source = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
escaped = source.replace("\\", "\\\\").replace('"', '\\"').splitlines()
out = pathlib.Path(sys.argv[2])
out.parent.mkdir(parents=True, exist_ok=True)
with out.open("w", encoding="utf-8", newline="\n") as f:
    f.write("// GENERATED; do not edit.\n#pragma once\n")
    f.write("namespace matter_cuda_embedded {\ninline const char invert_ptx[] =\n")
    for line in escaped:
        f.write(f'    "{line}\\n"\n')
    f.write("    ;\n}  // namespace matter_cuda_embedded\n")
