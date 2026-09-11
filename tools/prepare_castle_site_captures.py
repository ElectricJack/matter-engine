#!/usr/bin/env python3
"""Prepare geometry-checked castle screenshot timelines; never launch the editor.

Requires Node.js, NumPy and SciPy. Example from WSL:
  python3 tools/prepare_castle_site_captures.py --output-dir /mnt/d/tmp/castle-final-captures

The generated launch-native.cmd is run separately with native Windows Python.
The camera manifest records CPU checks, not visual acceptance of rendered frames.
"""

import argparse
import hashlib
import importlib.util
import itertools
import json
import math
import pathlib
import subprocess
import tempfile


def native_path(path, override=None):
    if override:
        return override.replace("\\", "/").rstrip("/")
    value = pathlib.Path(path).resolve().as_posix()
    if value.startswith("/mnt/") and len(value) > 7 and value[6] == "/":
        return value[5].upper() + ":" + value[6:]
    if len(value) > 2 and value[1] == ":":
        return value
    raise ValueError(
        "Native Windows path cannot be inferred; supply --native-output-dir / --native-repo"
    )


def source_snapshot(root):
    files = []
    for relative in (
        "projects/world_demo/shared-lib",
        "projects/world_demo/scenes",
        "projects/world_demo/objects",
        "MatterEngine3/shared-lib",
    ):
        files.extend((root / relative).rglob("*.js"))
    files.extend(
        [
            root / "tools/castle_scene_capture.py",
            root / "projects/world_demo/tests/castle_shared_lib_hooks.mjs",
            pathlib.Path(__file__).resolve(),
        ]
    )
    return {
        str(f.relative_to(root)): hashlib.sha256(f.read_bytes()).hexdigest()
        for f in sorted(files)
    }


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--output-dir", type=pathlib.Path, required=True)
    parser.add_argument(
        "--native-output-dir",
        help="Windows path to the same output directory; inferred for WSL drives",
    )
    parser.add_argument(
        "--native-repo", help="Windows path to this checkout; inferred for WSL drives"
    )
    parser.add_argument("--node", default="node")
    parser.add_argument(
        "--timeout",
        type=int,
        default=7200,
        help="Per-world native capture timeout in seconds",
    )
    args = parser.parse_args()
    try:
        import numpy as np
        from scipy.spatial import ConvexHull
        from scipy.spatial.transform import Rotation
    except ImportError as error:
        parser.error("CPU geometry validation requires NumPy and SciPy: " + str(error))
    ROOT = pathlib.Path(__file__).resolve().parent.parent
    BASE = args.output_dir.resolve()
    WIN = native_path(BASE, args.native_output_dir)
    NATIVE_ROOT = native_path(ROOT, args.native_repo)
    if any(c.isspace() for c in WIN):
        parser.error(
            "--native-output-dir must be whitespace-free for the screenshot FIFO grammar"
        )
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    BASE.mkdir(parents=True, exist_ok=True)
    source_before = source_snapshot(ROOT)
    with tempfile.TemporaryDirectory(prefix="castle-camera-geometry-") as working:
        DATA = pathlib.Path(working)
        inspector = DATA / "inspect.mjs"
        inspector.write_text(INSPECTOR, encoding="utf-8")
        with (DATA / "compiled-positions.txt").open("w", encoding="utf-8") as output:
            subprocess.run(
                [args.node, str(inspector), str(ROOT), str(DATA)],
                check=True,
                stdout=output,
                timeout=180,
            )
        NAMES = ["CastleClusteredCourt", "CastleAngledBailey", "CastleBentPalace"]
        fixture_data = [
            json.loads(l.split(" ", 1)[1])
            for l in (DATA / "compiled-positions.txt").read_text().splitlines()
        ]

        def transform(frame, p):
            a = math.radians(frame["yawDeg"])
            c, s = math.cos(a), math.sin(a)
            x, y, z = p
            return np.array(frame["origin"]) + np.array(
                [c * x + s * z, y, -s * x + c * z]
            )

        def corners(lo, hi):
            return np.array(list(itertools.product(*zip(lo, hi))))

        class Geometry:
            def __init__(self, d, i):
                self.records = []
                for e in d["world"]["entities"]:
                    c = e["components"]
                    t = c["LocalTransform"]
                    r = Rotation.from_quat(t.get("rotation", [0, 0, 0, 1]))
                    scale = np.array(t.get("scale", [1, 1, 1]))
                    if "BoxCollider" in c:
                        h = np.array(c["BoxCollider"]["halfExtents"])
                        v = corners(-h, h)
                    elif "ConvexHullCollider" in c:
                        v = np.array(c["ConvexHullCollider"]["points"]).reshape(-1, 3)
                    else:
                        continue
                    self.add(e["id"], r.apply(v * scale) + t["translation"])
                for w in fixture_data[i]["wings"]:
                    for f in w["fixtures"]:
                        if f["mount"] == "floor":
                            continue  # already in actual world colliders
                        b = f["bounds"]
                        v = corners(
                            [b["min" + a] for a in "XYZ"], [b["max" + a] for a in "XYZ"]
                        )
                        self.add(
                            w["id"] + ":fixture:" + f["id"],
                            np.array([transform(w["frame"], p) for p in v]),
                        )
                self.lo = np.array([r[1] for r in self.records])
                self.hi = np.array([r[2] for r in self.records])

            def add(self, id, v):
                hull = ConvexHull(v)
                self.records.append((id, v.min(axis=0), v.max(axis=0), hull.equations))

            def contains(self, p, margin=0.2):
                p = np.array(p)
                which = np.flatnonzero(
                    np.all(p >= self.lo - margin, axis=1)
                    & np.all(p <= self.hi + margin, axis=1)
                )
                return [
                    self.records[i][0]
                    for i in which
                    if np.max(self.records[i][3][:, :3] @ p + self.records[i][3][:, 3])
                    <= margin
                ]

            def ray(self, p, d, limit=1000):
                p = np.array(p)
                d = np.array(d)
                d = d / np.linalg.norm(d)
                parallel = abs(d) <= 1e-10
                inv = np.divide(1.0, d, out=np.zeros(3), where=~parallel)
                aa = (self.lo - p) * inv
                bb = (self.hi - p) * inv
                aa[:, parallel] = -np.inf
                bb[:, parallel] = np.inf
                outside_parallel = np.any(
                    ((p < self.lo) | (p > self.hi)) & parallel, axis=1
                )
                low = np.max(np.minimum(aa, bb), axis=1)
                high = np.min(np.maximum(aa, bb), axis=1)
                hits = []
                for i in np.flatnonzero(
                    (high >= np.maximum(low, 0)) & (low <= limit) & ~outside_parallel
                ):
                    n = self.records[i][3][:, :3]
                    q = self.records[i][3][:, 3]
                    v = n @ d
                    b = n @ p + q
                    if np.any((abs(v) < 1e-9) & (b > 1e-7)):
                        continue
                    neg = v < -1e-9
                    pos = v > 1e-9
                    enter = max(
                        0, float(np.max(-b[neg] / v[neg])) if np.any(neg) else 0
                    )
                    leave = min(
                        limit, float(np.min(-b[pos] / v[pos])) if np.any(pos) else limit
                    )
                    if enter <= leave + 1e-7:
                        hits.append((round(enter, 5), self.records[i][0]))
                return min(hits, default=None)

            def check(self, eye, target, walk=False):
                blockers = self.contains(eye)
                assert not blockers, (eye, blockers)
                eye, target = np.array(eye), np.array(target)
                delta = target - eye
                distance = float(np.linalg.norm(delta))
                ray = self.ray(eye, delta)
                assert ray is None or ray[0] > 0.8, (
                    "camera immediately faces geometry",
                    eye,
                    target,
                    ray,
                )
                ground = self.ray(eye, [0, -1, 0], 3)
                if walk:
                    assert ground and abs(ground[0] - 1.65) < 0.06, (
                        "walk-height camera lacks real floor",
                        eye,
                        ground,
                    )
                # Sample a 3x3 near-view cone: floor/furniture at ordinary viewing distances
                # are expected; reject camera corners clipping surfaces within 25cm.
                forward = delta / distance
                right = np.cross(forward, [0, 1, 0])
                right /= np.linalg.norm(right)
                up = np.cross(right, forward)
                near = []
                for x, y in itertools.product([-0.6, 0, 0.6], repeat=2):
                    hit = self.ray(
                        eye,
                        forward
                        + right * x * math.tan(math.pi / 8) * 16 / 9
                        + up * y * math.tan(math.pi / 8),
                        0.25,
                    )
                    if hit:
                        near.append(hit)
                assert not near, ("view near plane obstructed", near)
                return {
                    "eyeClearanceMetres": 0.2,
                    "nearViewRays": 9,
                    "nearViewClearMetres": 0.25,
                    "centralRayFirstHit": ray,
                    "targetDistanceMetres": round(distance, 5),
                    "groundBelowEye": ground,
                }

        manifest = {
            "sourceHashes": source_before,
            "schema": "matter.castle-capture-cameras/v1",
            "status": "prepared; GPU visual validation still required",
            "verticalFovDegrees": 45,
            "resolution": [1920, 1080],
            "geometryChecks": "Actual compiled world BoxCollider OBBs and ConvexHullCollider hulls; all wall/ceiling glazing and fixtures also checked using catalogue bounds. Fine brick chips and roof shingles are outside collider envelopes.",
            "worlds": [],
        }
        allpoints = []
        allrecords = []
        for i, name in enumerate(NAMES):
            d = json.loads((DATA / f"{i}-geometry.json").read_text())
            g = Geometry(d, i)
            wings = {w["id"]: w for w in d["site"]["wings"]}
            program = {w["id"]: w for w in d["program"]["wings"]}
            points = np.concatenate(
                [
                    np.array(
                        [
                            transform(w["frame"], p)
                            for p in corners(
                                [0, 0, 0],
                                [
                                    program[w["id"]]["plan"]["wingProgram"]["width"],
                                    program[w["id"]]["plan"]["levels"][-1]["baseY"] + 8,
                                    program[w["id"]]["plan"]["wingProgram"]["depth"],
                                ],
                            )
                        ]
                    )
                    for w in wings.values()
                ]
            )
            lo, hi = points.min(axis=0), points.max(axis=0)
            center = (lo + hi) / 2
            extent = max(hi[0] - lo[0], hi[2] - lo[2])
            offset = np.array([[-85, 0, 0], [0, 0, 0], [95, 0, 0]][i])
            allpoints.extend(points + offset)
            poses = []

            def add(id, eye, target, note, walk=False, lighting="daylight"):
                eye = list(map(float, eye))
                target = list(map(float, target))
                poses.append(
                    {
                        "id": id,
                        "eye": eye,
                        "target": target,
                        "note": note,
                        "lighting": lighting,
                        "walkHeight": walk,
                        "checks": g.check(eye, target, walk),
                    }
                )

            # Distinct opposite-side panorama chosen to reveal each site's internal court.
            vector = np.array(
                [[0.9, 0.72, -1.05], [0.8, 0.78, -1.0], [-1.05, 0.68, -0.8]][i]
            )
            add(
                "01-exterior",
                center + vector * extent * 1.15,
                [center[0], 6, center[2]],
                "Elevated three-quarter view showing connected angled wings and complete roofs.",
            )
            court = d["site"]["courtyards"][-1 if i == 2 else 0]
            p = np.mean(court["clearPolygon"], axis=0)
            targets = [[13.0, 5, 11], [-10, 6, 17], [9, 5, 29]]
            add(
                "02-courtyard",
                [p[0], 1.65, p[1]],
                targets[i],
                "Standing on compiled stone courtyard paving, looking up at enclosing wings.",
                True,
            )
            frame = wings["hall"]["frame"]
            add(
                "03-hall",
                transform(frame, [1.15, 1.65, 4]),
                transform(frame, [7, 2.9, 4.8]),
                "Great hall at eye height: chandelier, furniture and north glazing; identical pose for raster/RT.",
                True,
                "interior",
            )
            add(
                "04-hall-gallery",
                transform(frame, [1.05, 5.65, 4]),
                transform(frame, [7, 3.7, 4.8]),
                "Standing on upper west gallery: close gold chandelier detail, glazed openings and furnished hall below.",
                True,
                "interior",
            )
            frame = wings["chapel"]["frame"]
            add(
                "05-gold-glass",
                transform(frame, [4, 1.65, 7.8]),
                transform(frame, [3.15, 1.8, 12]),
                "View through real nave/chancel arch toward gold altar, low chandelier and stained glass.",
                True,
                "interior",
            )
            record = {
                "world": name,
                "inputGeometrySha256": hashlib.sha256(
                    (DATA / f"{i}-geometry.json").read_bytes()
                ).hexdigest(),
                "colliderCount": len(g.records),
                "bounds": {"min": lo.tolist(), "max": hi.tolist()},
                "poses": poses,
            }
            manifest["worlds"].append(record)
            print(
                name,
                [(p["id"], p["checks"]["centralRayFirstHit"]) for p in poses],
                flush=True,
            )
        points = np.array(allpoints)
        lo, hi = points.min(axis=0), points.max(axis=0)
        center = (lo + hi) / 2
        # Wide axis lies nearly horizontal on screen. 45deg vertical FOV gives72.7deg horizontal.
        galleryeye = np.array([center[0] + 12, 100, center[2] - 155])
        target = np.array([center[0], 4, center[2]])
        manifest["worlds"].append(
            {
                "world": "CastleSiteGallery",
                "bounds": {"min": lo.tolist(), "max": hi.tolist()},
                "poses": [
                    {
                        "id": "01-gallery-overview",
                        "eye": galleryeye.tolist(),
                        "target": target.tolist(),
                        "lighting": "daylight",
                        "note": "All three castles visible left-to-right, with enough elevation to read the distinct floor plans.",
                        "checks": {
                            "eyeAboveAllGeometryMetres": float(galleryeye[1] - hi[1])
                        },
                    }
                ],
            }
        )
        for world in manifest["worlds"]:
            out = BASE / world["world"]
            out.mkdir(exist_ok=True)
            lines = []
            for p in world["poses"]:
                interior = p["lighting"] == "interior"
                lines.extend(
                    [
                        "# " + p["note"],
                        "cam " + " ".join(f"{v:.6f}" for v in p["eye"] + p["target"]),
                        "set render.lighting.exposure_ev 0",
                        "set render.lighting.sun_multiplier "
                        + ("0.35" if interior else "1"),
                        "set render.lighting.sky_multiplier "
                        + ("0.45" if interior else "1"),
                        "set render.lighting.sky_irradiance_multiplier "
                        + ("0.45" if interior else "1"),
                        "set render.gi.enabled true",
                    ]
                )
                for mode, settle in [("raster", 60), ("native_rt", 180)]:
                    shot = f"{WIN}/{world['world']}/{p['id']}-{'rt' if mode == 'native_rt' else 'raster'}.png"
                    lines.extend(
                        [
                            "render_path " + mode,
                            "wait_frames " + str(settle),
                            "shot " + shot,
                        ]
                    )
            lines.append("quit")
            (out / "timeline.txt").write_text("\n".join(lines) + "\n")
            world["timeline"] = f"{WIN}/{world['world']}/timeline.txt"
            world["command"] = subprocess.list2cmdline(
                [
                    "py",
                    "-3",
                    NATIVE_ROOT + "/tools/castle_scene_capture.py",
                    "--world",
                    world["world"],
                    "--editor",
                    NATIVE_ROOT + "/MatterEditor/build/windows-msvc/editor.exe",
                    "--editor-dir",
                    NATIVE_ROOT + "/MatterEditor",
                    "--timeline",
                    world["timeline"],
                    "--out-dir",
                    WIN + "/" + world["world"],
                    "--timeout",
                    str(args.timeout),
                    "--env",
                    "MATTER_WINDOW_WIDTH=1920",
                    "--env",
                    "MATTER_WINDOW_HEIGHT=1080",
                    "--env",
                    "MATTER_VK_STATIC_RESERVE_VERTEX_MB=4096",
                    "--env",
                    "MATTER_VK_STATIC_RESERVE_INDEX_MB=512",
                ]
            )

        (BASE / "camera-manifest.json").write_text(
            json.dumps(manifest, indent=2) + "\n"
        )
        (BASE / "launch-native.cmd").write_text(
            "@echo off\r\n"
            + "".join(
                w["command"] + "\r\nif errorlevel 1 exit /b %errorlevel%\r\n"
                for w in manifest["worlds"]
            )
        )
        print(
            "Prepared",
            sum(len(w["poses"]) * 2 for w in manifest["worlds"]),
            "screenshots",
        )

        spec = importlib.util.spec_from_file_location(
            "capture", ROOT / "tools/castle_scene_capture.py"
        )
        capture = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(capture)
        m = json.loads((BASE / "camera-manifest.json").read_text())
        fixtures = [
            json.loads(l.split(" ", 1)[1])
            for l in (DATA / "compiled-positions.txt").read_text().splitlines()
        ]

        def project(pose, points):
            eye = np.array(pose["eye"])
            forward = np.array(pose["target"]) - eye
            forward /= np.linalg.norm(forward)
            right = np.cross(forward, [0, 1, 0])
            right /= np.linalg.norm(right)
            up = np.cross(right, forward)
            d = np.array(points) - eye
            z = d @ forward
            z[z <= 0] = np.nan  # Behind-camera points never count as framed features.
            return np.column_stack(
                [
                    d @ right / z / (math.tan(math.pi / 8) * 16 / 9),
                    d @ up / z / math.tan(math.pi / 8),
                ]
            )

        def transform(frame, p):
            a = math.radians(frame["yawDeg"])
            c, s = math.cos(a), math.sin(a)
            x, y, z = p
            return np.array(frame["origin"]) + [c * x + s * z, y, -s * x + c * z]

        shots = 0
        summary = []
        for i, w in enumerate(m["worlds"]):
            commands, paths = capture.timeline_commands(
                BASE / w["world"] / "timeline.txt"
            )
            shots += len(paths)
            assert len(paths) == len(set(paths)) == len(w["poses"]) * 2
            assert all(p.startswith(WIN + "/" + w["world"] + "/") for p in paths)
            assert sum(c == "render_path raster" for c in commands) == len(w["poses"])
            assert sum(c == "render_path native_rt" for c in commands) == len(
                w["poses"]
            )
            for pose in w["poses"]:
                if "exterior" in pose["id"] or "overview" in pose["id"]:
                    b = w["bounds"]
                    points = [
                        [x, y, z]
                        for x in [b["min"][0], b["max"][0]]
                        for y in [b["min"][1], b["max"][1]]
                        for z in [b["min"][2], b["max"][2]]
                    ]
                    xy = project(pose, points)
                    assert np.max(abs(xy)) < 1, (w["world"], xy)
                    pose["checks"]["conservativeSiteBoundsNdc"] = {
                        "min": xy.min(axis=0).tolist(),
                        "max": xy.max(axis=0).tolist(),
                    }
                if i == 3 or pose["id"] not in [
                    "03-hall",
                    "04-hall-gallery",
                    "05-gold-glass",
                ]:
                    continue
                wing = next(
                    f
                    for f in fixtures[i]["wings"]
                    if f["id"]
                    == ("chapel" if pose["id"] == "05-gold-glass" else "hall")
                )
                visible = []
                for f in wing["fixtures"]:
                    b = f["bounds"]
                    point = [
                        (b["minX"] + b["maxX"]) / 2,
                        (b["minY"] + b["maxY"]) / 2,
                        (b["minZ"] + b["maxZ"]) / 2,
                    ]
                    if f["kind"] == "chandelier":
                        point[1] = b["minY"] + 0.15
                    if f["kind"] in ["table", "altar"]:
                        point[1] = b["maxY"] - 0.1
                    xy = project(pose, [transform(wing["frame"], point)])[0]
                    if np.max(abs(xy)) < 1:
                        visible.append(
                            {
                                "kind": f["kind"],
                                "id": f["id"],
                                "featureNdc": xy.tolist(),
                            }
                        )
                pose["checks"]["featuresInFrustum"] = visible
                kinds = {f["kind"] for f in visible}
                if pose["id"] == "05-gold-glass":
                    assert {"altar", "window", "chandelier"} <= kinds, (
                        w["world"],
                        pose["id"],
                        kinds,
                    )
                elif pose["id"] == "04-hall-gallery":
                    assert {"chandelier", "window"} <= kinds, (
                        w["world"],
                        pose["id"],
                        kinds,
                    )
                else:
                    assert {"chandelier", "window"} <= kinds and kinds.intersection(
                        ["table", "bench", "chair"]
                    ), (w["world"], pose["id"], kinds)
                summary.append([w["world"], pose["id"], sorted(kinds)])
        (BASE / "camera-manifest.json").write_text(json.dumps(m, indent=2) + "\n")
        print(
            "Helper timeline contract: PASS;",
            shots,
            "screenshots; actual catalogue focal features inside45-degree frusta:",
            json.dumps(summary),
        )

        source_after = source_snapshot(ROOT)
        changed = [
            f
            for f in set(source_before) | set(source_after)
            if source_before.get(f) != source_after.get(f)
        ]
        if changed:
            (BASE / "camera-manifest.json").unlink(missing_ok=True)
            (BASE / "launch-native.cmd").unlink(missing_ok=True)
            raise RuntimeError(
                "Source changed during camera preparation; rerun after edits: "
                + ", ".join(sorted(changed))
            )
        print("Camera manifest:", BASE / "camera-manifest.json")
        print("Prepared native launch commands:", BASE / "launch-native.cmd")


INSPECTOR = r"""
import fs from 'node:fs';
import {pathToFileURL} from 'node:url';
const [rootPath,out]=process.argv.slice(2);
const root=pathToFileURL(rootPath+'/').href.replace(/\/$/,'');
await import(root+'/projects/world_demo/tests/castle_shared_lib_hooks.mjs');
globalThis.defineMaterial=(name)=>name;
const {castleSceneSite,castleSiteWorldDefinition}=await import(root+'/projects/world_demo/shared-lib/castle_site_world.js');
const {castleFurnishingLayout}=await import(root+'/projects/world_demo/shared-lib/castle_furnishing_layout.js');
const {castleSiteProgram,CASTLE_SITE_NAMES}=await import(root+'/projects/world_demo/shared-lib/castle_site_catalog.js');
for(let i=0;i<3;i++){
 const s=castleSceneSite(i),w=castleSiteWorldDefinition(CASTLE_SITE_NAMES[i]);
 fs.writeFileSync(out+'/'+i+'-geometry.json',JSON.stringify({site:s,program:castleSiteProgram(i),world:w},null,2));
 console.log(CASTLE_SITE_NAMES[i],JSON.stringify({camera:w.camera,spawn:s.spawn,courtyards:s.courtyards,wings:s.wings.map(wing=>({id:wing.id,frame:wing.frame,rooms:wing.manifest.rooms,fixtures:castleFurnishingLayout(wing.manifest).placements.map(p=>({id:p.id,kind:p.kind,mount:p.mount,transform:p.transform,bounds:p.footprint.aabb,params:p.params}))}))}));
}
"""

if __name__ == "__main__":
    main()
