#!/usr/bin/env python3
"""Prepare or sequentially run all staircase routes and missing connector detours.

CPU preparation (Node.js required):
  python3 tools/castle_site_walkthrough_batch.py --output-root /mnt/d/tmp/castle-physical-acceptance --prepare-only
Native acceptance (Windows Python, prebuilt PhysX editor):
  py -3 tools/castle_site_walkthrough_batch.py --output-root D:/tmp/castle-physical-acceptance --run

Every invocation exports fresh manifests. Each route uses the unmodified
walkthrough proof rules, a separate editor process, and a new evidence directory.
The runner stops at the first failure or changed source/binary snapshot.
Preparation is not native acceptance. Thirteen stair routes and seven detours
cover all stairs and connector mouths; other unvisited doors remain explicit.
"""

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
DRIVER = ROOT / "tools/castle_walkthrough_acceptance.py"
WORLDS = ["CastleClusteredCourt", "CastleAngledBailey", "CastleBentPalace"]
COUNTS = [4, 5, 4]
SUPPLEMENTAL_COUNTS = [2, 1, 4]
DETOURS = ROOT / "tools/castle_connector_route_overrides.mjs"
RESERVES = {
    "MATTER_VK_STATIC_RESERVE_VERTEX_MB": "4096",
    "MATTER_VK_STATIC_RESERVE_INDEX_MB": "512",
}


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write(path, value):
    Path(path).write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def snapshot():
    files = []
    for relative in [
        "projects/world_demo/shared-lib",
        "projects/world_demo/objects",
        "projects/world_demo/scenes",
        "MatterEngine3/shared-lib",
    ]:
        files.extend((ROOT / relative).rglob("*.js"))
    files.extend(
        [
            DRIVER,
            DETOURS,
            Path(__file__).resolve(),
            ROOT / "tools/castle_scene_capture.py",
            ROOT / "projects/world_demo/tests/castle_shared_lib_hooks.mjs",
        ]
    )
    return {str(f.relative_to(ROOT)): sha(f) for f in sorted(files)}


def native(path):
    value = Path(path).resolve().as_posix()
    if value.startswith("/mnt/") and len(value) > 7 and value[6] == "/":
        return value[5].upper() + ":" + value[6:]
    if len(value) > 2 and value[1] == ":":
        return value
    raise ValueError("Use a Windows drive or WSL /mnt/<drive> path for native evidence")


def driver_args(manifest, world, stair, output, args):
    return [
        "--manifest",
        str(manifest),
        "--world",
        world,
        "--stair",
        stair,
        "--output",
        str(output),
        "--editor",
        str(args.editor),
        "--cmake-cache",
        str(args.cmake_cache),
        "--player",
        "river-player",
        "--height",
        "1.8",
        "--radius",
        "0.4",
        "--speed",
        "4.5",
        "--timeout",
        str(args.timeout),
        "--static-vertex-reserve-mb",
        "4096",
        "--static-index-reserve-mb",
        "512",
        "--run",
    ]


def load_driver():
    spec = importlib.util.spec_from_file_location("castle_batch_acceptance", DRIVER)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def verify_override_input(route):
    if (
        route.get("route_override")
        and sha(route["route_override"]) != route["route_override_sha256"]
    ):
        raise RuntimeError(
            "Connector override changed before or during native traversal"
        )


def verify_connector_detour(
    driver, manifest, normalized, connector_id, plan, route_file
):
    override = json.loads(Path(route_file).read_text())
    if (
        override.get("connector_id") != connector_id
        or override.get("source_manifest_sha256") != plan["manifest_sha256"]
    ):
        raise RuntimeError(
            "Connector override provenance does not match its selected manifest/connector"
        )
    expected = {
        p["id"] for p in normalized["portals"] if p.get("connector_id") == connector_id
    }
    if len(expected) != 2 or not expected <= {p["id"] for p in plan["portals"]}:
        raise RuntimeError(
            "Connector detour does not prove both selected finite mouth planes: "
            + connector_id
        )
    connector = next(c for c in manifest["connectors"] if c["id"] == connector_id)
    sequence = connector["routeWaypoints"]
    if not driver.route_has_sequence(
        plan["waypoints"], sequence
    ) or not driver.route_has_sequence(plan["waypoints"], list(reversed(sequence))):
        raise RuntimeError(
            "Connector detour shortened its authored outbound/return waypoint sequence"
        )
    if plan["route_override_sha256"] != sha(route_file):
        raise RuntimeError(
            "Connector override hash was not preserved by the acceptance driver"
        )


def prepare_batch(args, directory, source_hashes):
    manifests = directory / "manifests"
    manifests.mkdir()
    with tempfile.TemporaryDirectory(prefix="castle-site-export-") as tmp:
        script = Path(tmp) / "export.mjs"
        script.write_text(EXPORTER, encoding="utf-8")
        subprocess.run(
            [args.node, str(script), str(ROOT), str(manifests)], check=True, timeout=180
        )
    driver = load_driver()
    metadata = json.loads((manifests / "export.json").read_text())
    routes, coverage = [], []
    for world, expected_count, expected_detours in zip(
        WORLDS, COUNTS, SUPPLEMENTAL_COUNTS
    ):
        path = manifests / (world + ".json")
        raw = json.loads(path.read_text())
        normalized = driver.normalize_manifest(raw)
        if len(normalized["stairs"]) != expected_count:
            raise RuntimeError(
                f"{world}: expected {expected_count} actual stairs; found {len(normalized['stairs'])}"
            )
        actor = metadata[world]["player"]
        controller = actor["components"]["CharacterController"]
        if (controller["height"], controller["radius"], controller["moveSpeed"]) != (
            1.8,
            0.4,
            4.5,
        ):
            raise RuntimeError(
                world + ": authored player differs from acceptance controller"
            )
        expected_spawn = [
            v + (0.95 if axis == 1 else 0) for axis, v in enumerate(raw["spawn"])
        ]
        if not driver.near(
            actor["components"]["LocalTransform"]["translation"], expected_spawn
        ):
            raise RuntimeError(world + ": authored player is not at the manifest spawn")
        visited_portals, visited_stairs = set(), set()

        def add_route(stair, route_file=None, connector_id=None):
            slug = re.sub(r"[^a-zA-Z0-9_.-]+", "-", stair["id"])
            if connector_id is not None:
                slug = "connector-" + re.sub(r"[^a-zA-Z0-9_.-]+", "-", connector_id)
            run_dir = directory / "runs" / world / slug
            plan_path = directory / "plans" / world / (slug + ".json")
            plan_path.parent.mkdir(parents=True, exist_ok=True)
            options = SimpleNamespace(
                manifest=path,
                world=world,
                stair=stair["id"],
                route_file=route_file,
                start_waypoint=0,
                offset=[0, 0, 0],
                height=1.8,
                radius=0.4,
                speed=4.5,
                player="river-player",
            )
            plan = driver.prepare(options)
            if plan["stair_id"] != stair["id"]:
                raise RuntimeError("Ambiguous selected staircase " + stair["id"])
            if connector_id is not None:
                verify_connector_detour(
                    driver, raw, normalized, connector_id, plan, route_file
                )
            write(plan_path, plan)
            visited_stairs.update(plan["visited_stair_ids"])
            visited_portals.update(p["id"] for p in plan["portals"])
            arguments = driver_args(path, world, stair["id"], run_dir, args)
            if route_file is not None:
                arguments.extend(["--route-file", str(route_file)])
            native_arguments = [
                native(value)
                if index
                and arguments[index - 1]
                in (
                    "--manifest",
                    "--output",
                    "--editor",
                    "--cmake-cache",
                    "--route-file",
                )
                else value
                for index, value in enumerate(arguments)
            ]
            routes.append(
                dict(
                    world=world,
                    stair_id=stair["id"],
                    connector_id=connector_id,
                    route_override=str(route_file) if route_file else None,
                    route_override_sha256=sha(route_file) if route_file else None,
                    plan=str(plan_path),
                    manifest=str(path),
                    manifest_sha256=sha(path),
                    output=str(run_dir),
                    visited_stair_ids=plan["visited_stair_ids"],
                    internal_doors=[
                        p["id"]
                        for p in plan["portals"]
                        if p["proof_kind"] == "internal"
                    ],
                    connector_mouths=[
                        p["id"]
                        for p in plan["portals"]
                        if p["proof_kind"] == "connector-mouth"
                    ],
                    waypoint_count=len(plan["waypoints"]),
                    arguments=arguments,
                    native_command=subprocess.list2cmdline(
                        ["py", "-3", native(DRIVER), *native_arguments]
                    ),
                    status="prepared",
                    native_executed=False,
                )
            )

        for stair in normalized["stairs"]:
            add_route(stair)
        baseline_portals = set(visited_portals)
        missing = [
            c
            for c in raw["connectors"]
            if not {
                "connector:" + c["id"] + ":mouth:a",
                "connector:" + c["id"] + ":mouth:b",
            }
            <= baseline_portals
        ]
        if len(missing) != expected_detours:
            raise RuntimeError(
                f"{world}: expected {expected_detours} supplemental connectors; found {len(missing)}"
            )
        selected = min(
            (r for r in routes if r["world"] == world),
            key=lambda r: (r["waypoint_count"], r["stair_id"]),
        )
        stair = next(s for s in normalized["stairs"] if s["id"] == selected["stair_id"])
        requests = [
            dict(
                manifest=str(path),
                connector_id=c["id"],
                stair_id=stair["id"],
                output=str(
                    directory / "overrides" / world / ("connector-" + c["id"] + ".json")
                ),
            )
            for c in missing
        ]
        request_path = manifests / (world + "-connector-requests.json")
        write(request_path, requests)
        subprocess.run(
            [args.node, str(DETOURS), str(request_path)], check=True, timeout=120
        )
        for request in requests:
            add_route(stair, Path(request["output"]), request["connector_id"])
        all_stairs = {s["id"] for s in normalized["stairs"]}
        if visited_stairs != all_stairs:
            raise RuntimeError(world + ": route batch does not cover all actual stairs")
        groups = {}
        for kind in ("internal", "connector-mouth"):
            all_ids = {
                p["id"] for p in normalized["portals"] if p["proof_kind"] == kind
            }
            groups[kind] = dict(
                planned=sorted(all_ids & visited_portals),
                unvisited=sorted(all_ids - visited_portals),
            )
        if groups["connector-mouth"]["unvisited"]:
            raise RuntimeError(world + ": supplemental routes omit connector mouths")
        coverage.append(
            dict(
                world=world,
                all_stairs=sorted(all_stairs),
                supplemental_connector_ids=[c["id"] for c in missing],
                baseline_planned_portals=sorted(baseline_portals),
                visited_stairs=sorted(visited_stairs),
                portals=groups,
                note="Coverage is planned until every native route receipt passes.",
            )
        )
    if snapshot() != source_hashes:
        raise RuntimeError(
            "Authoring or acceptance sources changed during fresh manifest export"
        )
    return routes, coverage


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument(
        "--editor",
        type=Path,
        default=ROOT / "MatterEditor/build/windows-msvc/editor.exe",
    )
    parser.add_argument(
        "--cmake-cache",
        type=Path,
        default=ROOT
        / "MatterEditor/build/cmake/windows-msvc/relwithdebinfo/CMakeCache.txt",
    )
    parser.add_argument("--node", default="node")
    parser.add_argument(
        "--timeout", type=int, default=7200, help="per-route native timeout in seconds"
    )
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--prepare-only", action="store_true")
    mode.add_argument("--run", action="store_true")
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    if args.run and os.name != "nt":
        parser.error("--run requires native Windows Python; preparation works in WSL")
    args.output_root = args.output_root.resolve()
    args.editor = args.editor.resolve()
    args.cmake_cache = args.cmake_cache.resolve()
    if any(c.isspace() for c in native(args.output_root)):
        parser.error("native output path must contain no whitespace")
    args.output_root.mkdir(parents=True, exist_ok=True)
    directory = args.output_root / (
        time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
        + "-"
        + str(time.time_ns() % 1000000000)
    )
    directory.mkdir()
    receipt = dict(
        schema="matter.castle-site-walkthrough-batch/v1",
        status="preparing",
        native_executed=False,
        started_ns=time.time_ns(),
        output=str(directory),
        routes=[],
        static_buffer_reserve_env=RESERVES,
        source_hashes=snapshot(),
    )
    result_path = directory / "batch.json"
    write(result_path, receipt)
    try:
        routes, coverage = prepare_batch(args, directory, receipt["source_hashes"])
        receipt.update(status="prepared", routes=routes, coverage=coverage)
        launch_args = [
            "py",
            "-3",
            native(Path(__file__)),
            "--output-root",
            native(args.output_root),
            "--editor",
            native(args.editor),
            "--cmake-cache",
            native(args.cmake_cache),
            "--timeout",
            str(args.timeout),
            "--node",
            args.node,
            "--run",
        ]
        launcher = (
            "@echo off\r\nREM Regenerates manifests from current sources before walking.\r\n"
            + subprocess.list2cmdline(launch_args)
            + "\r\n"
        )
        (directory / "launch-native.cmd").write_bytes(launcher.encode("utf-8"))
        (directory / "route-commands.txt").write_text(
            "\n".join(r["native_command"] for r in routes) + "\n", encoding="utf-8"
        )
        write(result_path, receipt)
        if args.run:
            if not args.editor.is_file() or not args.cmake_cache.is_file():
                raise RuntimeError("Matching native editor and CMake cache must exist")
            if not re.search(
                r"^MATTER_ENABLE_PHYSX:BOOL=ON$", args.cmake_cache.read_text(), re.M
            ):
                raise RuntimeError("Matching CMake cache must enable PhysX")
            binaries = {str(p): sha(p) for p in (args.editor, args.cmake_cache)}
            receipt.update(status="running", native_inputs_sha256=binaries)
            for route in routes:
                if snapshot() != receipt["source_hashes"] or any(
                    sha(p) != value for p, value in binaries.items()
                ):
                    raise RuntimeError(
                        "Source, editor or CMake cache changed during the batch"
                    )
                manifest = Path(route["manifest"])
                if sha(manifest) != route["manifest_sha256"]:
                    raise RuntimeError(
                        "Exported manifest changed before route execution"
                    )
                verify_override_input(route)
                route["started_ns"] = time.time_ns()
                receipt["native_attempted"] = True
                route["status"] = "running"
                write(result_path, receipt)
                print("Running", route["world"], route["stair_id"], flush=True)
                completed = subprocess.run(
                    [sys.executable, str(DRIVER), *route["arguments"]], check=False
                )
                path = Path(route["output"]) / "result.json"
                route["result"] = str(path)
                route["process_exit"] = completed.returncode
                if not path.is_file():
                    raise RuntimeError(
                        "Walkthrough driver produced no native receipt: "
                        + route["stair_id"]
                    )
                native_result = json.loads(path.read_text())
                route.update(
                    result_sha256=sha(path),
                    status=native_result.get("status"),
                    native_executed=native_result.get("native_executed", False),
                )
                receipt["native_executed"] |= route["native_executed"]
                if (
                    completed.returncode
                    or route["status"] != "passed"
                    or not route["native_executed"]
                ):
                    raise RuntimeError(
                        "Native route failed: "
                        + route["world"]
                        + ":"
                        + route["stair_id"]
                    )
                if (
                    native_result.get("manifest_sha256") != route["manifest_sha256"]
                    or native_result.get("static_buffer_reserve_env") != RESERVES
                    or native_result.get("render_path") != "raster"
                    or native_result.get("impostors_enabled") is not False
                    or native_result.get("world") != route["world"]
                    or native_result.get("editor_sha256") != binaries[str(args.editor)]
                    or native_result.get("cmake_cache_sha256")
                    != binaries[str(args.cmake_cache)]
                ):
                    raise RuntimeError(
                        "Native receipt used different inputs or reservation"
                    )
                native_plan = Path(route["output"]) / "route.json"
                if json.loads(native_plan.read_text()) != json.loads(
                    Path(route["plan"]).read_text()
                ):
                    raise RuntimeError(
                        "Native walkthrough used a different prepared route"
                    )
                route["native_plan_sha256"] = sha(native_plan)
                verify_override_input(route)
                if snapshot() != receipt["source_hashes"] or any(
                    sha(p) != value for p, value in binaries.items()
                ):
                    raise RuntimeError(
                        "Source, editor or CMake cache changed during native route"
                    )
                write(result_path, receipt)
            receipt["status"] = "passed"
        print("Batch receipt:", result_path)
        print("Regenerating native launcher:", directory / "launch-native.cmd")
        print("Prepared route commands:", directory / "route-commands.txt")
        return 0
    except Exception as error:
        receipt.update(status="failed", error=str(error))
        print("Castle batch failed:", error, file=sys.stderr)
        return 1
    finally:
        receipt["finished_ns"] = time.time_ns()
        write(result_path, receipt)


EXPORTER = r"""
import fs from 'node:fs';
import {pathToFileURL} from 'node:url';
const [repo,out]=process.argv.slice(2),root=pathToFileURL(repo+'/').href.replace(/\/$/,'');
await import(root+'/projects/world_demo/tests/castle_shared_lib_hooks.mjs');
globalThis.defineMaterial=name=>name;
const {castleSceneSite,castleSiteWorldDefinition}=await import(root+'/projects/world_demo/shared-lib/castle_site_world.js');
const names=['CastleClusteredCourt','CastleAngledBailey','CastleBentPalace'];
const keys=['clustered-court','angled-bailey','bent-palace'];
const metadata={};
for(let i=0;i<names.length;i++){
 const site=castleSceneSite(i),world=castleSiteWorldDefinition(keys[i]);
 fs.writeFileSync(out+'/'+names[i]+'.json',JSON.stringify(site));
 metadata[names[i]]={player:world.entities.find(e=>e.id==='river-player')};
}
fs.writeFileSync(out+'/export.json',JSON.stringify(metadata));
"""

if __name__ == "__main__":
    sys.exit(main())
