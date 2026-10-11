"""Audit the actual standalone CMake target graph; no extraction scaffold counts."""
import json
from pathlib import Path
import sys
import shlex
import subprocess


def audit(build):
    reply = build / ".cmake/api/v1/reply"
    indices = sorted(reply.glob("index-*.json"))
    assert indices, "Missing CMake File API reply"
    index = json.loads(indices[-1].read_text())
    model = json.loads((reply / index["reply"]["codemodel-v2"]["jsonFile"]).read_text())
    root = Path(model["paths"]["source"]).resolve()
    repository = root.parent.parent
    allowed = [root, repository / "libs/MatterContracts", repository / "libs/MathLib/include", build]
    legacy_files = {repository / "MatterEngine3/src/render/matrix_math.cpp",
                    repository / "MatterEngine3/src/render/matrix_math.h",
                    repository / "MatterEngine3/include/matter/math_types.h"}
    legacy_includes = {repository / "MatterEngine3/src/render", repository / "MatterEngine3/include"}
    expected = {"matter_contracts", "matter_graphics", "matter_graphics_tests",
                "matter_graphics_header_backend", "matter_graphics_header_graphics",
                "matter_graphics_header_scene_backend", "matter_graphics_scene_tests"}
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
        archives = set()
        for t in targets.values():
            if t["type"] == "STATIC_LIBRARY":
                archives.update((build / a["path"]).resolve() for a in t.get("artifacts", []))
        for target in targets.values():
            for artifact in target.get("artifacts", []):
                assert (build / artifact["path"]).is_file(), f"Target was not built: {target['name']}"
            paths = [Path(s["path"]) for s in target.get("sources", [])]
            for path in paths:
                # File API relative sources are relative to the source root.
                path = (path if path.is_absolute() else root / path).resolve()
                assert path in legacy_files or any(path.is_relative_to(base.resolve()) for base in allowed), f"Forbidden source: {path}"
            for group in target.get("compileGroups", []):
                for include in group.get("includes", []):
                    path = Path(include["path"]).resolve()
                    assert path in legacy_includes or any(path.is_relative_to(base.resolve()) for base in allowed), f"Forbidden include directory: {path}"
            # Inspect actual linker fragments too: direct library paths need
            # not appear as CMake target dependencies.
            system_libraries = {"kernel32.lib", "user32.lib", "gdi32.lib", "winspool.lib",
                                "shell32.lib", "ole32.lib", "oleaut32.lib", "uuid.lib",
                                "comdlg32.lib", "advapi32.lib"}
            for fragment in target.get("link", {}).get("commandFragments", []):
                if fragment["role"] != "libraries":
                    continue
                for library in shlex.split(fragment["fragment"].replace("\\", "/")):
                    assert library.lower() in system_libraries or (build / library).resolve() in archives, f"Forbidden link input: {library}"
    # Ninja's compiler-discovered dependencies catch forbidden transitive
    # headers hidden behind the two broad legacy include search directories.
    cache = (build / "CMakeCache.txt").read_text()
    ninja = next(line.split("=", 1)[1] for line in cache.splitlines() if line.startswith("CMAKE_MAKE_PROGRAM:"))
    deps = subprocess.run([ninja, "-C", str(build), "-t", "deps"], check=True, capture_output=True, text=True).stdout
    assert "#deps" in deps, "No compiled dependency evidence; build before auditing"
    checked = set()
    for line in deps.splitlines():
        if not line.startswith("    "):
            continue
        path = Path(line.strip())
        path = (path if path.is_absolute() else build / path).resolve()
        if path.is_relative_to(repository):
            assert path in legacy_files or any(path.is_relative_to(base.resolve()) for base in allowed), f"Forbidden compiler dependency: {path}"
            checked.add(path)
    # MSVC /showIncludes omits the primary .cpp; the codemodel above audits it.
    required_headers = {p for p in legacy_files if p.suffix == ".h"}
    assert required_headers <= checked, f"Shared renderer math headers not compiled: {required_headers - checked}"
    print("PASS GFX-INDEPENDENT-GRAPH: scene/facade/contracts plus shared pure renderer math; source, link and compiler dependency audit")


if __name__ == "__main__":
    audit(Path(sys.argv[1]).resolve())
