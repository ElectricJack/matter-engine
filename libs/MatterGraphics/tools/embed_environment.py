"""Adapt the existing native SPIR-V generator to a module-private namespace."""
from pathlib import Path
import runpy
import sys

generator = Path(__file__).resolve().parents[3] / "MatterEngine3/tools/embed_spirv.py"
render = runpy.run_path(str(generator))["render"]
content = render([Path(name) for name in sys.argv[2:]])
content = content.replace("namespace matter {", "namespace matter::graphics::environment_spirv {")
Path(sys.argv[1]).write_text(content, encoding="utf-8")
