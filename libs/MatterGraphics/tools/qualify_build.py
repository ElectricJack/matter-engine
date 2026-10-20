"""Build-time audit of the actual standalone graph and shader dependency closure.

This reads build metadata deliberately and therefore belongs to the build,
after all compiled targets, rather than the disk-free runtime CTest tier.
"""
import json
from pathlib import Path
import sys
import shlex
import subprocess
from functools import lru_cache


@lru_cache(maxsize=None)
def resolved(path):
    # Compiler dependencies repeat the same headers across many translation
    # units. Resolve each path once, preserving the symlink-aware boundary.
    return path.resolve()


def audit(build):
    reply = build / ".cmake/api/v1/reply"
    indices = sorted(reply.glob("index-*.json"))
    assert indices, "Missing CMake File API reply"
    index = json.loads(indices[-1].read_text())
    model = json.loads((reply / index["reply"]["codemodel-v2"]["jsonFile"]).read_text())
    root = resolved(Path(model["paths"]["source"]))
    repository = root.parent.parent
    allowed = [root, repository / "libs/MatterContracts", repository / "libs/MathLib/include", build]
    cache = (build / "CMakeCache.txt").read_text()
    settings = {line.split(":", 1)[0]: line.split("=", 1)[1] for line in cache.splitlines()
                if ":" in line and "=" in line and not line.startswith(("//", "#"))}
    vulkan = settings.get("MATTER_GRAPHICS_VULKAN") == "ON"
    native_libraries = set()
    if vulkan:
        allowed.append(resolved(Path(settings["Vulkan_INCLUDE_DIR"])))
        loader = resolved(Path(settings["Vulkan_LIBRARY"]))
        assert loader.is_file() and loader.name.lower().startswith(("vulkan-1.lib", "libvulkan.so")), loader
        native_libraries.add(loader)
    legacy_files = {repository / "MatterEngine3/include/matter/math_types.h"}
    if vulkan:
        legacy_files.add(repository / "libs/ProfileLib/include/profile.h")
        shader_root = repository / "MatterEngine3/shaders_vk"
        environment_shaders = tuple(name + ".comp" for name in (
            "atmosphere_transmittance", "atmosphere_multiscatter", "atmosphere_sky_view",
            "atmosphere_irradiance", "cloud_shadow_density", "cloud_shadow_integrate",
            "cloud_shadow_reproject", "vol_density", "vol_scatter", "vol_integrate"))
        allowed_shaders = {shader_root / name for name in (
            "composite.vert", "composite.frag", "display_transform.frag",
            "vol_common.glsl", "impostor_common.glsl", "environment_common.glsl",
            "cloud_shadow_common.glsl", "local_lighting.glsl")}
        # The moved local_lighting implementation includes its sibling types
        # directly, so the engine's local_light_types wrapper is not in the closure.
        allowed_shaders.update(root / "shaders" / name for name in
                               ("impostor_common.glsl", "local_lighting.glsl", "local_light_types.glsl"))
        allowed_shaders.update(shader_root / name for name in environment_shaders + (
            "atmosphere_common.glsl", "cloud_density.glsl", "vt_noise.glsl"))
        raster_shaders = ("raster.vert", "raster_skin.vert", "raster_water.vert", "gbuffer.frag",
                          "visibility_id.vert", "visibility_id.frag")
        allowed_shaders.update(shader_root / name for name in (
            "visibility_id.vert", "visibility_id.frag", "tileset_common.glsl", "surface_detail.glsl", "wang_common.glsl"))
        allowed_shaders.update(root / "shaders" / name for name in (
            "raster.vert", "gbuffer.frag", "material_common.glsl", "vt_chart_types.glsl", "vt_common.glsl",
            "vt_draw_inputs.glsl", "vt_normal_frame.glsl", "vt_parallax.glsl", "vt_seed_walk.glsl",
            "vt_surface_walk.glsl", "vt_visible_input.glsl", "water_surface.glsl"))
        legacy_files.update(allowed_shaders)
        # Exact native platform/data closure; never grant the whole engine tree.
        legacy_files.update(repository / "MatterEngine3" / name for name in (
            "src/render/vk_resources.h",
            "src/render/vk_cloud_shadows.h", "src/render/vk_volumetrics.h",
            "src/render/frame_matrices.h", "src/render/gpu_matrix_pack.h",
            "include/matter/vulkan_device.h", "include/matter/camera.h",
            "include/matter/atmosphere.h", "include/matter/atmosphere_lighting.h",
            "include/matter/cloud_layers.h", "include/matter/cloud_shadow_settings.h",
            "include/matter/volumetric_quality.h", "include/matter/display_dither.h",
            "include/matter/sun_angles.h", "include/matter/log.h", "include/matter/compiler.h",
            "tests/check.h", "tests/atmosphere_tests.cpp", "tests/cloud_layer_tests.cpp",
            "tests/cloud_shadow_tests.cpp", "tests/volumetric_quality_tests.cpp"))
    legacy_includes = {repository / "MatterEngine3/include"}
    if vulkan:
        legacy_includes.add(repository / "libs/ProfileLib/include")
    expected = {"matter_contracts", "matter_graphics", "matter_graphics_tests",
                "matter_graphics_header_backend", "matter_graphics_header_graphics",
                "matter_graphics_header_scene_backend", "matter_graphics_scene_tests",
                "matter_graphics_header_composition", "matter_graphics_composition_tests"}
    if vulkan:
        expected.update({"matter_graphics_vulkan", "matter_graphics_sample", "matter_graphics_vulkan_tests", "matter_graphics_raster_tests", "matter_graphics_header_vulkan_resources",
                         "matter_graphics_header_vulkan_composition", "matter_graphics_header_vulkan_raster"})
        expected.add("matter_graphics_environment")
        expected.add("matter_graphics_cloud_shadow_reference_tests")
        expected.update("matter_graphics_" + suite + "_tests" for suite in (
            "atmosphere", "cloud_layer", "cloud_shadow", "volumetric_quality"))
    allowed = [resolved(base) for base in allowed]
    for config in model["configurations"]:
        all_targets = {t["name"]: json.loads((reply / t["jsonFile"]).read_text()) for t in config["targets"]}
        utilities = {name for name, t in all_targets.items() if t["type"] == "UTILITY"}
        # include(CTest) registers dashboard commands, not linked libraries.
        assert all(name in {"matter_graphics_qualification", "matter_graphics_environment_qualification"} or
                   name.startswith(("Nightly", "Continuous", "Experimental"))
                   for name in utilities), utilities
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
                archives.update(resolved(build / a["path"]) for a in t.get("artifacts", []))
        for target in targets.values():
            for artifact in target.get("artifacts", []):
                assert (build / artifact["path"]).is_file(), f"Target was not built: {target['name']}"
            paths = [Path(s["path"]) for s in target.get("sources", [])]
            for path in paths:
                # File API relative sources are relative to the source root.
                path = resolved(path if path.is_absolute() else root / path)
                assert path in legacy_files or any(path.is_relative_to(base) for base in allowed), f"Forbidden source: {path}"
            for group in target.get("compileGroups", []):
                for include in group.get("includes", []):
                    path = resolved(Path(include["path"]))
                    assert path in legacy_includes or any(path.is_relative_to(base) for base in allowed), f"Forbidden include directory: {path}"
            # Inspect actual linker fragments too: direct library paths need
            # not appear as CMake target dependencies.
            system_libraries = {"kernel32.lib", "user32.lib", "gdi32.lib", "winspool.lib",
                                "shell32.lib", "ole32.lib", "oleaut32.lib", "uuid.lib",
                                "comdlg32.lib", "advapi32.lib"}
            for fragment in target.get("link", {}).get("commandFragments", []):
                if fragment["role"] != "libraries":
                    continue
                for library in shlex.split(fragment["fragment"].replace("\\", "/")):
                    assert library.lower() in system_libraries or resolved(build / library) in archives | native_libraries, f"Forbidden link input: {library}"
    # Ninja's compiler-discovered dependencies catch forbidden transitive
    # headers hidden behind the two broad legacy include search directories.
    ninja = next(line.split("=", 1)[1] for line in cache.splitlines() if line.startswith("CMAKE_MAKE_PROGRAM:"))
    deps = subprocess.run([ninja, "-C", str(build), "-t", "deps"], check=True, capture_output=True, text=True).stdout
    assert "#deps" in deps, "No compiled dependency evidence; build before auditing"
    checked = set()
    for line in deps.splitlines():
        if not line.startswith("    "):
            continue
        path = Path(line.strip())
        path = resolved(path if path.is_absolute() else build / path)
        if path.is_relative_to(repository):
            assert path in legacy_files or any(path.is_relative_to(base) for base in allowed), f"Forbidden compiler dependency: {path}"
            checked.add(path)
    # MSVC /showIncludes omits the primary .cpp; the codemodel above audits it.
    required_headers = {p for p in legacy_files if p.suffix == ".h"}
    assert required_headers <= checked, f"Shared renderer math headers not compiled: {required_headers - checked}"
    if vulkan:
        shader_deps = set()
        for shader in ("composite.vert", "composite.frag", "display_transform.frag") + environment_shaders + raster_shaders:
            raw = (build / (shader + ".spv.d")).read_text()
            # compile_shader.py escapes paths; split only the target delimiter.
            shader_deps.update(resolved(Path(p)) for p in shlex.split(raw.split(": ", 1)[1]))
        assert shader_deps == allowed_shaders, f"Unexpected shader dependency closure: {shader_deps ^ allowed_shaders}"
    print("PASS GFX-INDEPENDENT-GRAPH: scene/facade/contracts/shared pure math" +
          (" plus isolated Vulkan adapter/loader and exact production shader closure" if vulkan else "") +
          "; source, link and compiler dependency audit")


if __name__ == "__main__":
    audit(Path(sys.argv[1]).resolve())
