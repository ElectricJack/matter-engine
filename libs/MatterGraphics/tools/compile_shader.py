"""Compile source-of-truth GLSL and write a space-safe CMake/Ninja depfile."""
from pathlib import Path
import subprocess
import sys

compiler, source_arg, output_arg = sys.argv[1:]
source, output = Path(source_arg).resolve(), Path(output_arg).resolve()
depfile = Path(str(output) + ".d")
# glslc emits unescaped spaces in dependencies. Relative shader/include names
# keep its output unambiguous even when the monorepo path contains spaces.
subprocess.run([compiler, "--target-env=vulkan1.2", "-I.", "-MD", "-MF", str(depfile),
                "-MT", output.name, source.name, "-o", str(output)],
               cwd=source.parent, check=True)
raw = depfile.read_text().replace("\\\n", " ")
dependencies = raw.split(": ", 1)[1].split()

def escaped(path):
    return str(path).replace("\\", "/").replace("$", "$$").replace("#", "\\#").replace(" ", "\\ ").replace(":", "\\:")

paths = [(source.parent / name).resolve() for name in dependencies]
if not paths or any(not p.is_relative_to(source.parent) or not p.is_file() for p in paths):
    raise ValueError("Shader dependencies must remain within the shader source directory")
depfile.write_text(escaped(output) + ": " + " ".join(map(escaped, paths)) + "\n")
