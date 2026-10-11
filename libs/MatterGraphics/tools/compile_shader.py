"""Compile source-of-truth GLSL and write a space-safe CMake/Ninja depfile."""
from pathlib import Path
import subprocess
import sys
import os

compiler, source_arg, output_arg = sys.argv[1:4]
source, output = Path(source_arg).resolve(), Path(output_arg).resolve()
roots = [source.parent]
options = []
for arg in sys.argv[4:]:
    if arg.startswith('-D'):
        options.append(arg)
    else:
        root = Path(arg).resolve()
        roots.append(root)
        options.append('-I' + os.path.relpath(root, source.parent).replace('\\', '/'))
depfile = Path(str(output) + ".d")
# glslc emits unescaped spaces in dependencies. Relative shader/include names
# keep its output unambiguous even when the monorepo path contains spaces.
subprocess.run([compiler, "--target-env=vulkan1.2", *options, "-I.", "-MD", "-MF", str(depfile),
                "-MT", output.name, source.name, "-o", str(output)],
               cwd=source.parent, check=True)
raw = depfile.read_text().replace("\\\n", " ")
dependencies = raw.split(": ", 1)[1].split()

def escaped(path):
    return str(path).replace("\\", "/").replace("$", "$$").replace("#", "\\#").replace(" ", "\\ ").replace(":", "\\:")

paths = [(source.parent / name).resolve() for name in dependencies]
if not paths or any(not any(p.is_relative_to(root) for root in roots) or not p.is_file() for p in paths):
    raise ValueError("Shader dependencies must remain within the explicit shader roots")
depfile.write_text(escaped(output) + ": " + " ".join(map(escaped, paths)) + "\n")
