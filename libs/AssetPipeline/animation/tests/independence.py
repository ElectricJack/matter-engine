"""Build static generation and portable animation separately; audit actual sources/links."""
import argparse
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--make-program")
    parser.add_argument("--build-root", type=Path, required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[4]
    cases = [
        ("portable", "AssetPipeline/animation", "matter_animation_asset_tests", [
            "-DBUILD_TESTING=ON", "-DMATTER_ANIMATION_NATIVE=OFF"]),
        ("static", "ProceduralGeometry", "matter_geometry_recipe", [
            "-DBUILD_TESTING=OFF", "-DMATTER_GEOMETRY_TESTS=OFF"]),
    ]
    for name, module, executable, options in cases:
        build = (args.build_root / name).resolve()
        query = build / ".cmake/api/v1/query"
        query.mkdir(parents=True, exist_ok=True)
        (query / "codemodel-v2").touch()
        command = [args.cmake, "-S", str(repo / "libs" / module), "-B", str(build),
                   "-G", "Ninja", "-DCMAKE_BUILD_TYPE=RelWithDebInfo", *options]
        if args.make_program:
            command.append("-DCMAKE_MAKE_PROGRAM=" + args.make_program)
        subprocess.run(command, check=True)
        subprocess.run([args.cmake, "--build", str(build), "--target", executable,
                        "--parallel", "2"], check=True)
        program = build / executable
        if not program.exists():
            program = program.with_suffix(".exe")
        subprocess.run([str(program)], check=True)
        reply = build / ".cmake/api/v1/reply"
        index = json.loads(max(reply.glob("index-*.json")).read_text())
        model = json.loads((reply / index["reply"]["codemodel-v2"]["jsonFile"]).read_text())
        targets = model["configurations"][0]["targets"]
        names = {t["name"] for t in targets}
        assert executable in names and "matter_contracts" in names, names
        assert not any("ozz" in n.lower() or "animation_native" in n for n in names), names
        if name == "static":
            assert not any("animation" in n for n in names), names
        sources = []
        for ref in targets:
            target = json.loads((reply / ref["jsonFile"]).read_text())
            for entry in target.get("sources", []):
                source = entry["path"].replace("\\", "/")
                sources.append(source)
                assert not any(s in source.lower() for s in (
                    "/quickjs/", "/ozz-animation/", "/script_host.cpp", "/matter_engine.cpp",
                    "/animation_store.cpp", "/animation_systems.cpp", "/dsl_state.cpp")), source
                if name == "static":
                    # Generated files live below the module's matrix directory;
                    # their location is not a dependency on animation sources.
                    if not entry.get("isGenerated") and not Path(source).is_relative_to(build):
                        assert "/animation/" not in source and "/component_adapters/animation/" not in source, source
        (build / "animation-independence.json").write_text(json.dumps({
            "fixture": "ANIM-INDEPENDENCE-" + name,
            "targets": sorted(names), "sources": sorted(sources),
        }, indent=2) + "\n")
        print("ANIM-INDEPENDENCE-" + name + ": PASS", flush=True)


if __name__ == "__main__":
    main()
