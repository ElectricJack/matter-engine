"""Audit the actual standalone CMake target graph; no extraction scaffold counts."""
import json
from pathlib import Path
import sys


def audit(build):
    reply = build / ".cmake/api/v1/reply"
    indices = sorted(reply.glob("index-*.json"))
    assert indices, "Missing CMake File API reply"
    index = json.loads(indices[-1].read_text())
    model = json.loads((reply / index["reply"]["codemodel-v2"]["jsonFile"]).read_text())
    root = Path(model["paths"]["source"]).resolve()
    repository = root.parent.parent
    allowed = [root, repository / "libs/MatterContracts", repository / "libs/MathLib/include", build]
    expected = {"matter_contracts", "matter_graphics", "matter_graphics_tests",
                "matter_graphics_header_backend", "matter_graphics_header_graphics"}
    for config in model["configurations"]:
        all_targets = {t["name"]: json.loads((reply / t["jsonFile"]).read_text()) for t in config["targets"]}
        utilities = {name for name, t in all_targets.items() if t["type"] == "UTILITY"}
        # include(CTest) registers dashboard commands, not linked libraries.
        assert all(name.startswith(("Nightly", "Continuous", "Experimental")) for name in utilities), utilities
        targets = {name: t for name, t in all_targets.items() if name not in utilities}
        assert set(targets) == expected, f"Unexpected transitive targets: {set(targets) ^ expected}"
        assert targets["matter_graphics"]["type"] == "STATIC_LIBRARY", "Facade must compile runtime code"
        assert targets["matter_contracts"]["type"] == "STATIC_LIBRARY"
        names = {t["id"]: name for name, t in targets.items()}
        dependencies = {names[d["id"]] for d in targets["matter_graphics"].get("dependencies", [])}
        assert dependencies == {"matter_contracts"}, f"Forbidden graphics dependencies: {dependencies}"
        for target in targets.values():
            paths = [Path(s["path"]) for s in target.get("sources", [])]
            paths += [Path(i["path"]) for group in target.get("compileGroups", []) for i in group.get("includes", [])]
            for path in paths:
                # File API relative sources are relative to the source root.
                path = (path if path.is_absolute() else root / path).resolve()
                assert any(path.is_relative_to(base.resolve()) for base in allowed), f"Forbidden source/include: {path}"
    print("PASS GFX-INDEPENDENT-GRAPH: compiled facade/contracts only; no procedural, terrain, host or monolith")


if __name__ == "__main__":
    audit(Path(sys.argv[1]).resolve())
