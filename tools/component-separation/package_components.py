#!/usr/bin/env python3
"""Build, stage and verify the local component packages in packages.json.

Each native package is configured standalone from its own source directory with
the rules.json probe options, built in Release, and staged as include/, lib/,
docs, examples, THIRD_PARTY_NOTICES.txt, a generated CMake package config and a
matter-package.json file list. A package is rejected when the third-party files
its compiler actually read (ninja's recorded header deps) disagree with its
declared notices, or when it links a target that no package owns or that
rules.json does not allow. Every package is then unpacked with only its
required packages into an empty prefix outside the checkout, and existing
component sources are built and run against that prefix alone.

JavaScript packages are packed with npm; --emsdk also builds the browser WASM
package and the standalone viewer demo bundle. Nothing is published.
"""
import argparse
import hashlib
import importlib.util
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.request
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from functools import partial
from pathlib import Path
from threading import Thread

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def load_stager():
    spec = importlib.util.spec_from_file_location(
        "matter_stager", ROOT / "tools/stage-windows-msvc-package.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


STAGER = load_stager()


def run(command, **kwargs):
    print("RUN:", " ".join(str(part) for part in command), flush=True)
    return subprocess.run([str(part) for part in command], check=True, **kwargs)


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def git(*arguments):
    return subprocess.check_output(["git", "-C", str(ROOT), *arguments], text=True).strip()


def alias(target):
    return "matter::" + target.removeprefix("matter_")


def notice_source(key, entry, emsdk):
    if "stager" in entry:
        _, rel, mode = next(c for c in STAGER.NOTICE_COMPONENTS if c[0] == entry["stager"])
        return ROOT / rel, mode, rel
    if "sdk" in entry:
        if emsdk is None:
            raise SystemExit(f"notice {key} needs --emsdk")
        return emsdk / entry["sdk"], entry["mode"], "emsdk/" + entry["sdk"]
    return ROOT / entry["path"], entry["mode"], entry["path"]


def write_notices(path, keys, notices, emsdk):
    parts = [f"Third-party notices for {path.parent.name}.",
             "Matter's own sources in this package are repository-internal and carry no separate notice.",
             "Listed components were compiled, linked or shipped into this package; nothing else was."]
    if not keys:
        parts.append("\nThis package contains no third-party code.")
    for key in sorted(keys):
        source, mode, label = notice_source(key, notices[key], emsdk)
        note = notices[key].get("note")
        parts.append(f"\n=== {key} ({label}) ===" + (f"\n[{note}]" if note else ""))
        parts.append(STAGER.read_notice(source, mode))
    path.write_text("\n".join(parts) + "\n", encoding="utf-8")


def codemodel(build):
    reply = build / ".cmake/api/v1/reply"
    index = json.loads(max(reply.glob("index-*.json")).read_text())
    model = json.loads((reply / index["reply"]["codemodel-v2"]["jsonFile"]).read_text())
    targets = {}
    for entry in model["configurations"][0]["targets"]:
        target = json.loads((reply / entry["jsonFile"]).read_text())
        target["deps"] = [dep["id"].split("::")[0] for dep in target.get("dependencies", [])]
        targets[target["name"]] = target
    return targets


def third_party_inputs(build, targets):
    """Every third_party file the compiler read for these targets' objects."""
    out = subprocess.check_output(["ninja", "-C", str(build), "-t", "deps"], text=True)
    markers = tuple(f"CMakeFiles/{target}.dir/" for target in targets)
    found, current = set(), False
    for line in out.splitlines():
        if line and not line[0].isspace():
            current = any(marker in line.split(":", 1)[0] for marker in markers)
        elif current and line.strip():
            path = (build / line.strip()).resolve()
            if path.is_relative_to(ROOT / "third_party"):
                found.add(path.relative_to(ROOT).as_posix())
    return found


def match_notices(inputs, declared, notices, label):
    used = {key for key in notices for path in inputs
            if any(path.startswith(prefix) for prefix in notices[key]["covers"])}
    uncovered = sorted(p for p in inputs if not any(
        p.startswith(prefix) for key in notices for prefix in notices[key]["covers"]))
    if uncovered or used != set(declared):
        raise SystemExit(f"{label}: notice mismatch: declared {sorted(declared)}, inputs need "
                         f"{sorted(used)}, uncovered {uncovered}")


def project_version(package):
    text = (ROOT / package["source"] / "CMakeLists.txt").read_text()
    match = re.search(r"project\(\s*\S+\s+VERSION\s+([0-9.]+)", text)
    if match and package.get("version") not in (None, match.group(1)):
        raise SystemExit(f"{package['name']}: manifest version disagrees with CMake {match.group(1)}")
    if not match and "version" not in package:
        raise SystemExit(f"{package['name']}: no CMake project VERSION; declare one in packages.json")
    return (match.group(1), "cmake project()") if match else (package["version"], "packages.json")


def config_text(package, version, model, owners, requires):
    lines = ["# Generated by tools/component-separation/package_components.py.",
             "include(CMakeFindDependencyMacro)"]
    lines += [f"find_dependency({name} {owners_version} CONFIG)" for name, owners_version in requires]
    if "Threads::Threads" in package.get("system_links", []):
        lines.append("find_dependency(Threads)")
    lines.append('get_filename_component(_matter_prefix "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)')
    for target in package["targets"]:
        node = model[target]
        links = [alias(dep) for dep in node["deps"] if dep in owners]
        if target == package["targets"][0]:
            links += package.get("system_links", [])
        lines += [f"if(NOT TARGET {alias(target)})",
                  f"  add_library({alias(target)} STATIC IMPORTED)",
                  f"  set_target_properties({alias(target)} PROPERTIES",
                  f'    IMPORTED_LOCATION "${{_matter_prefix}}/lib/{node["artifacts"][0]["path"].split("/")[-1]}"',
                  '    INTERFACE_INCLUDE_DIRECTORIES "${_matter_prefix}/include"',
                  # a C consumer of a C++ archive must still link with the C++ driver
                  f'    IMPORTED_LINK_INTERFACE_LANGUAGES "{";".join(sorted({g["language"] for g in node["compileGroups"]}))}"',
                  f'    INTERFACE_LINK_LIBRARIES "{";".join(links)}"',
                  "    INTERFACE_COMPILE_FEATURES cxx_std_17)", "endif()"]
    if "aggregate" in package:
        members = ";".join(alias(t) for t in package["public_targets"])
        lines += [f"if(NOT TARGET {alias(package['aggregate'])})",
                  f"  add_library({alias(package['aggregate'])} INTERFACE IMPORTED)",
                  f'  set_target_properties({alias(package["aggregate"])} PROPERTIES INTERFACE_LINK_LIBRARIES "{members}")',
                  "endif()"]
    version_lines = [f'set(PACKAGE_VERSION "{version}")',
                     'if(PACKAGE_FIND_VERSION_MAJOR AND NOT PACKAGE_FIND_VERSION_MAJOR STREQUAL "' + version.split(".")[0] + '")',
                     "  set(PACKAGE_VERSION_COMPATIBLE FALSE)",
                     "elseif(PACKAGE_FIND_VERSION VERSION_GREATER PACKAGE_VERSION)",
                     "  set(PACKAGE_VERSION_COMPATIBLE FALSE)",
                     "else()", "  set(PACKAGE_VERSION_COMPATIBLE TRUE)",
                     "  if(PACKAGE_FIND_VERSION VERSION_EQUAL PACKAGE_VERSION)", "    set(PACKAGE_VERSION_EXACT TRUE)",
                     "  endif()", "endif()"]
    return "\n".join(lines) + "\n", "\n".join(version_lines) + "\n"


def copy_into(source, destination):
    if source.is_dir():
        for path in sorted(source.rglob("*")):
            if path.is_file():
                target = destination / path.relative_to(source)
                if target.exists():
                    raise SystemExit(f"two package inputs stage {target}")
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(path, target)
    else:
        destination.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination / source.name)


def file_list(stage):
    return {path.relative_to(stage).as_posix(): {"bytes": path.stat().st_size, "sha256": sha256(path)}
            for path in sorted(stage.rglob("*")) if path.is_file()}


def deterministic_tar(stage, archive, mtime):
    def clean(info):
        info.uid = info.gid = 0
        info.uname = info.gname = ""
        info.mtime = mtime
        return info
    with open(archive, "wb") as raw:
        import gzip
        with gzip.GzipFile(fileobj=raw, mode="wb", mtime=mtime) as zipped:
            with tarfile.open(fileobj=zipped, mode="w") as tar:
                for path in [stage, *sorted(stage.rglob("*"))]:
                    tar.add(path, arcname=Path(stage.name, path.relative_to(stage)).as_posix(),
                            recursive=False, filter=clean)


def build_native(package, context):
    rules = context["rules"]
    name, component = package["name"], package["component"]
    version, version_source = project_version(package)
    build = context["out"] / "build" / name
    shutil.rmtree(build, ignore_errors=True)
    (build / ".cmake/api/v1/query").mkdir(parents=True)
    (build / ".cmake/api/v1/query/codemodel-v2").touch()
    options = {**rules["probe_cache"], **package.get("cmake", {})}
    started = time.monotonic()
    run(["cmake", "-S", ROOT / package["source"], "-B", build, "-G", "Ninja",
         "-DCMAKE_BUILD_TYPE=Release", *[f"-D{k}={v}" for k, v in sorted(options.items())]],
        stdout=subprocess.DEVNULL)
    configured = time.monotonic()
    run(["cmake", "--build", build, "--target", *package["targets"]])
    built = time.monotonic()
    model = codemodel(build)
    owners = {t: p for p in context["manifest"]["native"] for t in p["targets"]}
    allowed = next(c for c in rules["components"] if c["name"] == component)["allowed_components"]
    requires = {}
    for target in package["targets"]:
        if model[target]["type"] != "STATIC_LIBRARY":
            raise SystemExit(f"{name}: {target} is {model[target]['type']}, not a static library")
        for dep in model[target]["deps"]:
            if model[dep]["type"] == "UTILITY" or dep in package["targets"]:
                continue
            owner = owners.get(dep)
            if owner is None:
                raise SystemExit(f"{name}: {target} links {dep}, which no package owns")
            if owner["component"] not in allowed:
                raise SystemExit(f"{name}: {target} links {dep} from {owner['component']}, "
                                 f"outside rules.json allowed_components {allowed}")
            requires[owner["name"]] = project_version(owner)[0]
    inputs = third_party_inputs(build, package["targets"])
    match_notices(inputs, package["notices"], context["manifest"]["notices"], name)

    stage = context["out"] / "stage" / f"{name}-{version}"
    shutil.rmtree(stage, ignore_errors=True)
    for include in package["include_dirs"]:
        copy_into(ROOT / include, stage / "include")
    for header in package.get("exclude_headers", []):
        (stage / "include" / header).unlink()  # a variant's library lacks these implementations
    for target in package["targets"]:
        copy_into(build / model[target]["artifacts"][0]["path"], stage / "lib")
    for doc in package["docs"]:
        copy_into(ROOT / doc, stage / "share/doc" / name / Path(doc).parent.relative_to(package["source"]))
    for example in package["examples"]:
        copy_into(ROOT / example, stage / "share/doc" / name / "examples")
    config, config_version = config_text(package, version, model, owners, sorted(requires.items()))
    cmake_dir = stage / "share/cmake" / name
    cmake_dir.mkdir(parents=True)
    (cmake_dir / f"{name}-config.cmake").write_text(config)
    (cmake_dir / f"{name}-config-version.cmake").write_text(config_version)
    write_notices(stage / "THIRD_PARTY_NOTICES.txt", package["notices"],
                  context["manifest"]["notices"], None)
    record = {"schema": "matter.component-package.v1", "name": name, "component": component,
              "version": version, "version_source": version_source,
              "variant": package.get("variant", "default"),
              "source": {"commit": context["commit"], "dirty": context["dirty"], "directory": package["source"]},
              "platform": context["platform"], "cmake_options": options,
              "targets": {alias(t): f"lib/{model[t]['artifacts'][0]['path'].split('/')[-1]}"
                          for t in package["targets"]},
              "requires": requires, "allowed_components": allowed,
              "system_links": package.get("system_links", []),
              "third_party_inputs": sorted(inputs), "notices": sorted(package["notices"])}
    record["files"] = file_list(stage)
    (stage / "matter-package.json").write_text(json.dumps(record, indent=2) + "\n")
    archive = context["out"] / f"{stage.name}-{context['platform']['tag']}.tar.gz"
    deterministic_tar(stage, archive, context["mtime"])
    lib_bytes = sum(v["bytes"] for k, v in record["files"].items() if k.startswith("lib/"))
    return {"name": name, "component": component, "version": version, "archive": archive.name,
            "archive_bytes": archive.stat().st_size, "archive_sha256": sha256(archive),
            "files": len(record["files"]) + 1, "library_bytes": lib_bytes,
            "headers": sum(k.startswith("include/") for k in record["files"]),
            "configure_seconds": round(configured - started, 2), "build_seconds": round(built - configured, 2),
            "requires": requires, "notices": sorted(package["notices"]), "third_party_inputs": sorted(inputs)}


def consume_native(package, results, context):
    """Build existing sources against the unpacked package closure only."""
    work = Path(tempfile.mkdtemp(prefix=f"{package['name']}-consumer-"))
    if work.resolve().is_relative_to(ROOT):
        raise SystemExit("consumer work directory must be outside the checkout")
    closure, pending = [], [package["name"]]
    while pending:
        current = pending.pop()
        if current not in closure:
            closure.append(current)
            pending += list(results[current]["requires"])
    for member in closure:
        target = work / "prefix"
        with tarfile.open(context["out"] / results[member]["archive"]) as tar:
            root = tar.getmembers()[0].name
            tar.extractall(work / "unpacked", filter="data")
        for path in sorted((work / "unpacked" / root).rglob("*")):
            if path.is_file():
                rel = path.relative_to(work / "unpacked" / root)
                if rel.parts[0] in ("include", "lib", "share"):
                    dest = target / rel
                    if dest.exists() and sha256(dest) != sha256(path):
                        raise SystemExit(f"{member} and an earlier package disagree on {rel}")
                    dest.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(path, dest)
    project = work / "project"
    project.mkdir()
    spec = package["consumer"]
    links = {link for program in spec["programs"] for link in program["links"]}
    lines = ["cmake_minimum_required(VERSION 3.25)", "project(matter_package_consumer LANGUAGES C CXX)",
             f"find_package({package['name']} {results[package['name']]['version']} CONFIG REQUIRED)"]
    if "Threads::Threads" in links:
        lines.append("find_package(Threads REQUIRED)")
    if "cmake_include" in spec:
        shutil.copy2(ROOT / spec["cmake_include"], project)
        lines.append(f"include(${{CMAKE_CURRENT_SOURCE_DIR}}/{Path(spec['cmake_include']).name})")
    for program in spec["programs"]:
        for item in program["sources"] + program.get("headers", []) + program.get("copy", []):
            shutil.copy2(ROOT / item, project)
        sources = " ".join(Path(s).name for s in program["sources"])
        lines += [f"add_executable({program['name']} {sources})",
                  f"target_link_libraries({program['name']} PRIVATE {' '.join(program['links'])})",
                  f"target_include_directories({program['name']} PRIVATE ${{CMAKE_CURRENT_BINARY_DIR}})",
                  f"target_compile_features({program['name']} PRIVATE cxx_std_17)"]
        if program.get("definitions"):
            lines.append(f"target_compile_definitions({program['name']} PRIVATE {' '.join(program['definitions'])})")
        if program.get("c_standard"):
            lines.append(f"set_target_properties({program['name']} PROPERTIES C_STANDARD {program['c_standard']})")
    (project / "CMakeLists.txt").write_text("\n".join(lines) + "\n")
    started = time.monotonic()
    build = work / "build"
    run(["cmake", "-S", project, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
         f"-DCMAKE_PREFIX_PATH={work / 'prefix'}", "-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF",
         "-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF"], stdout=subprocess.DEVNULL)
    run(["cmake", "--build", build])
    leaked = sorted(line.strip() for line in subprocess.check_output(
        ["ninja", "-C", str(build), "-t", "deps"], text=True).splitlines()
        if line.startswith(" ") and Path(line.strip()).resolve().is_relative_to(ROOT))
    if leaked:
        raise SystemExit(f"{package['name']} consumer read checkout files: {leaked[:5]}")
    outcomes = []
    for command in spec["runs"]:
        argv = [str(build / command[0]), *[a.replace("{work}", str(work)) for a in command[1:]]]
        result = subprocess.run(argv, cwd=work, capture_output=True, text=True, timeout=600)
        outcomes.append({"argv": [command[0], *command[1:]], "exit": result.returncode})
        if result.returncode:
            sys.stdout.write(result.stdout[-4000:] + result.stderr[-4000:])
            raise SystemExit(f"{package['name']} consumer {command} exited {result.returncode}")
    elapsed = round(time.monotonic() - started, 2)
    shutil.rmtree(work)
    return {"closure": closure, "runs": outcomes, "seconds": elapsed}


def pack_npm(source, out):
    listing = json.loads(subprocess.check_output(
        ["npm", "pack", "--json", "--pack-destination", str(out), str(source)], text=True))[0]
    return out / listing["filename"], [entry["path"] for entry in listing["files"]]


def build_browser(context, emsdk):
    """Build @matter/procedural-wasm and the viewer demo bundle, add notices, pack and check."""
    manifest, out = context["manifest"], context["out"]
    entries = {entry["name"]: entry for entry in manifest["javascript"]}
    wasm_build = out / "build/procedural-wasm"
    started = time.monotonic()
    run([sys.executable, ROOT / "web/matter-procedural-wasm/build.py", "--emsdk", emsdk,
         "--build-dir", wasm_build])
    wasm_seconds = round(time.monotonic() - started, 2)
    source = ROOT / "web/matter-procedural-wasm"
    stage = out / "stage/procedural-wasm"
    shutil.rmtree(stage, ignore_errors=True)
    for item in ["package.json", "README.md", "API.md", "dist"]:
        copy_into(source / item, stage / ("dist" if item == "dist" else ""))
    write_notices(stage / "dist/THIRD_PARTY_NOTICES.txt", entries["@matter/procedural-wasm"]["notices"],
                  manifest["notices"], emsdk)
    wasm_archive, wasm_files = pack_npm(stage, out)
    if "dist/THIRD_PARTY_NOTICES.txt" not in wasm_files:
        raise SystemExit("npm pack dropped the WASM package notices")
    work = Path(tempfile.mkdtemp(prefix="matter-wasm-consumer-"))
    with tarfile.open(wasm_archive) as tar:
        tar.extractall(work, filter="data")
    copy_into(source / "tests/package.test.mjs", work / "package/tests")
    run(["node", "--test", work / "package/tests/package.test.mjs"], cwd=work / "package")
    shutil.rmtree(work)

    viewer = out / "build/web-viewer"
    shutil.rmtree(viewer, ignore_errors=True)
    started = time.monotonic()
    run(["cmake", "-S", ROOT / "integrations/web-procedural/viewer", "-B", viewer, "-G", "Ninja",
         "-DBUILD_TESTING=OFF", f"-DMATTER_WEB_WASM_MODULE={source / 'dist/matter_geometry_wasm.mjs'}"],
        stdout=subprocess.DEVNULL)
    run(["cmake", "--build", viewer, "--target", "matter_web_demo"])
    demo_seconds = round(time.monotonic() - started, 2)
    demo = out / "stage/matter-web-demo"
    shutil.rmtree(demo, ignore_errors=True)
    copy_into(viewer / "demo-bundle", demo)
    write_notices(demo / "THIRD_PARTY_NOTICES.txt", entries["matter-web-demo"]["notices"],
                  manifest["notices"], emsdk)
    demo_archive = out / "matter-web-demo-0.1.0.tar.gz"
    deterministic_tar(demo, demo_archive, context["mtime"])
    served = serve_check(demo_archive)
    return [{"name": "@matter/procedural-wasm", "archive": wasm_archive.name, "archive_bytes": wasm_archive.stat().st_size,
             "archive_sha256": sha256(wasm_archive), "files": len(wasm_files), "build_seconds": wasm_seconds,
             "wasm_bytes": (source / "dist/matter_geometry_wasm.wasm").stat().st_size,
             "notices": sorted(entries["@matter/procedural-wasm"]["notices"]), "consumer": "node --test tests/package.test.mjs"},
            {"name": "matter-web-demo", "archive": demo_archive.name, "archive_bytes": demo_archive.stat().st_size,
             "archive_sha256": sha256(demo_archive), "files": len(file_list(demo)), "build_seconds": demo_seconds,
             "notices": sorted(entries["matter-web-demo"]["notices"]), "consumer": served}]


def serve_check(archive):
    """Serve the unpacked bundle over plain HTTP and fetch every file it ships."""
    work = Path(tempfile.mkdtemp(prefix="matter-web-demo-"))
    with tarfile.open(archive) as tar:
        root = tar.getmembers()[0].name
        tar.extractall(work, filter="data")
    bundle = work / root
    server = ThreadingHTTPServer(("127.0.0.1", 0), partial(SimpleHTTPRequestHandler, directory=str(bundle)))
    Thread(target=server.serve_forever, daemon=True).start()
    fetched = 0
    try:
        for path in sorted(p for p in bundle.rglob("*") if p.is_file()):
            url = f"http://127.0.0.1:{server.server_port}/{path.relative_to(bundle).as_posix()}"
            with urllib.request.urlopen(url) as response:
                if response.read() != path.read_bytes():
                    raise SystemExit(f"served bytes differ for {url}")
            fetched += 1
    finally:
        server.shutdown()
    shutil.rmtree(work)
    return f"python http.server served all {fetched} files byte-identical; rendering needs a browser"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path, default=ROOT / "artifacts/component-packages")
    parser.add_argument("--only", action="append", help="native package name (repeatable)")
    parser.add_argument("--emsdk", type=Path, help="also build the WASM package and the viewer demo bundle")
    parser.add_argument("--no-consumers", action="store_true")
    args = parser.parse_args()
    manifest = json.loads((HERE / "packages.json").read_text())
    rules = json.loads((HERE / "rules.json").read_text())
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    compiler = subprocess.check_output(["c++", "--version"], text=True).splitlines()[0]
    tag = f"{platform.system().lower()}-{platform.machine()}"
    context = {"manifest": manifest, "rules": rules, "out": out,
               "commit": git("rev-parse", "HEAD"), "dirty": bool(git("status", "--porcelain")),
               "mtime": int(git("log", "-1", "--format=%ct")),
               "platform": {"tag": tag, "compiler": compiler, "cmake": subprocess.check_output(
                   ["cmake", "--version"], text=True).splitlines()[0]}}
    selected = [p for p in manifest["native"] if not args.only or p["name"] in args.only]
    results = {}
    for package in manifest["native"]:
        needed = package in selected or any(package["name"] in results.get(s["name"], {}).get("requires", {})
                                            for s in selected)
        if needed or package in selected:
            results[package["name"]] = build_native(package, context)
    if not args.no_consumers:
        for package in selected:
            missing = [r for r in results[package["name"]]["requires"] if r not in results]
            if missing:
                raise SystemExit(f"{package['name']} requires unbuilt packages {missing}")
            results[package["name"]]["consumer"] = consume_native(package, results, context)
    javascript = []
    for entry in manifest["javascript"]:
        if entry["name"] == "@matter/authoring":
            archive, files = pack_npm(ROOT / entry["source"], out)
            work = Path(tempfile.mkdtemp(prefix="matter-authoring-consumer-"))
            with tarfile.open(archive) as tar:
                tar.extractall(work, filter="data")
            run(["node", "--input-type=module", "-e",
                 "const m = await import(process.argv[1]); const e = await import(process.argv[2]);"
                 "if (!m.RecipeBuilder || !e.orderedCsg) process.exit(1);",
                 (work / "package/index.mjs").as_uri(), (work / "package/examples/recipes.mjs").as_uri()])
            shutil.rmtree(work)
            javascript.append({"name": entry["name"], "archive": archive.name, "archive_bytes": archive.stat().st_size,
                               "archive_sha256": sha256(archive), "files": len(files), "contents": files,
                               "notices": [], "consumer": "unpacked index.mjs and examples/recipes.mjs import"})
    if args.emsdk:
        javascript += build_browser(context, args.emsdk.resolve())
    report = {"schema": "matter.component-package-metrics.v1", "source_commit": context["commit"],
              "dirty": context["dirty"], "platform": context["platform"],
              "native": list(results.values()), "javascript": javascript}
    (out / "metrics.json").write_text(json.dumps(report, indent=2) + "\n")
    for item in report["native"] + javascript:
        print(f"{item['name']:28} {item['archive_bytes']:>10} B  {item['files']:>4} files  {item['archive']}")
    print(f"metrics: {out / 'metrics.json'}")


if __name__ == "__main__":
    main()
