"""Compile source-of-truth GLSL and write a space-safe CMake/Ninja depfile."""
from pathlib import Path
import subprocess
import sys

compiler, source_arg, output_arg = sys.argv[1:4]
target_env = sys.argv[4] if len(sys.argv) > 4 else "vulkan1.2"
source, output = Path(source_arg).resolve(), Path(output_arg).resolve()
depfile = Path(str(output) + ".d")
# glslc emits unescaped spaces in dependencies. Relative shader/include names
# keep its output unambiguous even when the monorepo path contains spaces.
subprocess.run([compiler, "--target-env=" + target_env, "-I.", "-MD", "-MF", str(depfile),
                "-MT", output.name, source.name, "-o", str(output), *sys.argv[5:]],
               cwd=source.parent, check=True)
raw = depfile.read_text().replace("\\\n", " ")
dependencies = raw.split(": ", 1)[1].split()

def escaped(path):
    return str(path).replace("\\", "/").replace("$", "$$").replace("#", "\\#").replace(" ", "\\ ").replace(":", "\\:")

paths = [(source.parent / name).resolve() for name in dependencies]
graphics_shaders = Path(__file__).resolve().parent.parent / "shaders"
allowed_roots = (source.parent, graphics_shaders)
if not paths or any(not p.is_file() or not any(p.is_relative_to(root) for root in allowed_roots)
                    for p in paths):
    raise ValueError("Shader dependencies must remain within the production shader roots")
depfile.write_text(escaped(output) + ": " + " ".join(map(escaped, paths)) + "\n")
