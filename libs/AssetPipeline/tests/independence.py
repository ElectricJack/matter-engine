"""Build actual independent libraries and audit CMake's generated target graph."""
import argparse
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--make-program")
    parser.add_argument("--generator", default="Ninja")
    parser.add_argument("--build-root", type=Path, required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[3]
    adapters_off = ["-DMATTER_ASSET_STORAGE=OFF", "-DMATTER_ASSET_GLTF=OFF", "-DMATTER_ASSET_LEGACY=OFF"]
    kernels = {"matter_asset_lod_kernels", "matter_geometry"}
    # (name, module, required targets, options, extra forbidden targets)
    cases = [
        ("geometry", "ProceduralGeometry", {"matter_geometry"}, ["-DMATTER_GEOMETRY_TESTS=OFF"], set()),
        ("graphics", "MatterGraphics", {"matter_graphics"}, ["-DMATTER_GRAPHICS_VULKAN=OFF"], set()),
        # Prepared loading without any generation kernel.
        ("archive-only", "AssetPipeline", {"matter_asset_archive", "matter_asset_prepared"},
         adapters_off + ["-DMATTER_ASSET_LOD=OFF"], kernels),
        # LOD generation pulls the geometry stages but no I/O adapter.
        ("lod-only", "AssetPipeline", {"matter_asset_prepared", "matter_asset_lod"} | kernels,
         adapters_off, set()),
    ]
    for name, module, expected, options, excluded in cases:
        build = (args.build_root / name).resolve()
        query = build / ".cmake/api/v1/query"
        query.mkdir(parents=True, exist_ok=True)
        (query / "codemodel-v2").touch()
        command = [args.cmake, "-S", str(repo / "libs" / module), "-B", str(build),
                   "-G", args.generator, "-DBUILD_TESTING=OFF", "-DCMAKE_BUILD_TYPE=RelWithDebInfo", *options]
        if args.make_program:
            command.append("-DCMAKE_MAKE_PROGRAM=" + args.make_program)
        subprocess.run(command, check=True)
        subprocess.run([args.cmake, "--build", str(build), "--parallel", "2"], check=True)
        reply = build / ".cmake/api/v1/reply"
        index = json.loads(max(reply.glob("index-*.json")).read_text())
        model = json.loads((reply / index["reply"]["codemodel-v2"]["jsonFile"]).read_text())
        targets = model["configurations"][0]["targets"]
        names = {t["name"] for t in targets}
        assert expected <= names and "matter_contracts" in names, (name, names)
        forbidden = {"matter_asset_pipeline", "matter_asset_storage", "matter_asset_gltf", "matter_asset_legacy"}
        forbidden |= excluded
        if module != "AssetPipeline":
            forbidden |= {"matter_asset_archive", "matter_asset_prepared", "matter_asset_lod"}
        assert not names.intersection(forbidden), (name, names)
        if module != "AssetPipeline":
            for ref in targets:
                target = json.loads((reply / ref["jsonFile"]).read_text())
                for source in target.get("sources", []):
                    if source.get("isGenerated"):
                        continue
                    source_path = Path(source["path"])
                    if not source_path.is_absolute():
                        source_path = Path(model["paths"]["source"]) / source_path
                    source_path = source_path.resolve()
                    # configure_file output may lack isGenerated in File API.
                    # The matrix build directory is inside AssetPipeline on
                    # Windows; its generated geometry glue is not an adapter.
                    if source_path.is_relative_to(build):
                        continue
                    assert not source_path.is_relative_to(repo / "libs" / "AssetPipeline"), source
        (build / "asset-independence.json").write_text(json.dumps({"fixture": "AP-INDEPENDENCE-" + name,
            "targets": sorted(names), "storage_present": False}, indent=2) + "\n")
        print("AP-INDEPENDENCE-" + name + ": PASS", flush=True)


if __name__ == "__main__":
    main()
