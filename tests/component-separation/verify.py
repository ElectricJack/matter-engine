"""Data integrity and analytical oracles; this does not run a geometry producer."""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
import math
from pathlib import Path
import subprocess

CORPUS = Path(__file__).resolve().parent
REPO = CORPUS.parent.parent
REQUIRED_CASES = {
    "primitive", "extruded_prop", "ordered_csg", "repeated_child_assembly", "material_assignment",
    "mirrored_nonuniform_placement", "deterministic_seeded_variation", "malformed_recipe", "resource_exhaustion",
    "cancellation", "terrain_adjacent_chunks", "terrain_resolution_changes", "terrain_late_invalidation",
}


def load(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def near(actual, expected, rule, label):
    require(math.isfinite(actual) and math.isfinite(expected), label + ": nonfinite")
    require(abs(actual - expected) <= rule["absolute"] + rule["relative"] * abs(expected),
            f"{label}: {actual} != {expected}")


def subtract(a, b):
    return [x - y for x, y in zip(a, b)]


def cross(a, b):
    return [a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]]


def dot(a, b):
    return sum(x*y for x, y in zip(a, b))


def occupied(point, positions, triangles):
    # Oblique ray avoids the analytical fixtures' face diagonals and axes.
    direction = [0.731, 0.413, 0.527]
    hits = []
    for i, j, k in triangles:
        a, b, c = positions[i], positions[j], positions[k]
        e1, e2 = subtract(b, a), subtract(c, a)
        h = cross(direction, e2)
        det = dot(e1, h)
        if abs(det) < 1e-12:
            continue
        s = subtract(point, a)
        u = dot(s, h)/det
        q = cross(s, e1)
        v, t = dot(direction, q)/det, dot(e2, q)/det
        if u >= 0 and v >= 0 and u+v <= 1 and t > 1e-10:
            if not any(abs(t-prev) < 1e-9 for prev in hits):
                hits.append(t)
    return len(hits) % 2 == 1


def compare_geometry(asset, oracle, profile):
    require(len(asset["meshes"]) == 1, "one mesh oracle required")
    mesh = asset["meshes"][0]
    positions, indices = mesh["positions"], mesh["indices"]
    require(positions and len(indices) % 3 == 0, "nonempty triangle mesh required")
    require(all(len(p) == 3 and all(math.isfinite(x) for x in p) for p in positions), "finite vec3 positions required")
    require(all(type(i) is int and 0 <= i < len(positions) for i in indices), "index out of range")
    require(mesh["winding"] == "ccw", "CCW required")
    tolerance = profile["position_metres"]
    for axis in range(3):
        for side, fn in [("min", min), ("max", max)]:
            value = fn(p[axis] for p in positions)
            near(value, oracle["bounds"][side][axis], tolerance, "geometry bounds")
            near(mesh["bounds"][side][axis], value, tolerance, "declared bounds")
    # Weld exact duplicated seam vertices. Approximate vertex welding is a
    # producer-specific policy and is deliberately not inferred here.
    welded, lookup = [], {}
    for p in positions:
        key = tuple(p)
        lookup.setdefault(key, len(lookup))
        welded.append(lookup[key])
    triangles = [indices[i:i+3] for i in range(0, len(indices), 3)]
    directed, adjacent = Counter(), defaultdict(set)
    volume = 0
    for tri in triangles:
        a, b, c = [positions[i] for i in tri]
        require(dot(cross(subtract(b, a), subtract(c, a)), cross(subtract(b, a), subtract(c, a))) > 0,
                "degenerate triangle")
        volume += dot(a, cross(b, c))/6
        ids = [welded[i] for i in tri]
        for u, v in zip(ids, ids[1:] + ids[:1]):
            directed[u, v] += 1
            adjacent[u].add(v)
            adjacent[v].add(u)
    require(all(count == 1 and directed[v, u] == 1 for (u, v), count in directed.items()),
            "closed manifold with opposite edge directions required")
    near(volume, oracle["volume_metres3"], profile["volume_metres3"], "signed volume/winding")
    remaining, components = set(adjacent), 0
    while remaining:
        components += 1
        pending = [remaining.pop()]
        while pending:
            neighbors = adjacent[pending.pop()] & remaining
            remaining -= neighbors
            pending.extend(neighbors)
    require(components == oracle.get("shell_components", 1), "shell component count")
    require(set(indices) == set(range(len(positions))), "unused vertices")
    coverage = [None] * len(indices)
    for s in mesh["submeshes"]:
        first, count = s["first_index"], s["index_count"]
        require(first >= 0 and count > 0 and first % 3 == 0 and count % 3 == 0 and first+count <= len(indices), "submesh range")
        for i in range(first, first+count):
            require(coverage[i] is None, "overlapping submesh")
            coverage[i] = s["material"]
    require(all(m == oracle["material"] for m in coverage), "material binding")
    for q in oracle.get("occupancy", []):
        require(occupied(q["position"], positions, triangles) == q["inside"], "solid occupancy")
    return {"triangles": len(triangles), "volume_metres3": volume, "shell_components": components}


def terrain_boundary(terrain, tolerance):
    portable = terrain["portable_oracle"]
    coarse, fine = portable["coarse_boundary"], portable["fine_boundary"]
    require(len(fine) == 2*len(coarse)-1, "terrain transition sample counts")
    require(portable["transition"] == "fine odd samples are linear interpolation of neighboring coarse samples; both sides cover the same plane, no added skirt",
            "unsupported terrain transition policy")
    for i, p in enumerate(fine):
        a = coarse[i//2]
        expected = a if i % 2 == 0 else [(x+y)/2 for x, y in zip(a, coarse[i//2+1])]
        for x, y in zip(p, expected):
            near(x, y, tolerance, "shared terrain boundary")
    require(portable["chunks"][1]["local_origin"][0] == portable["chunks"][0]["size_metres"],
            "adjacent terrain origins")


def compare_export_text(actual, expected, profile, kind):
    def lines(text):
        return [line.split() for line in text.splitlines() if line.strip() and not line.lstrip().startswith("#")]
    got, want = lines(actual), lines(expected)
    require(len(got) == len(want), "export statement count")
    for a, b in zip(got, want):
        require(a[0] == b[0] and len(a) == len(b), "export statement shape")
        rule = None
        if kind == "obj":
            key = {"v": "position_metres", "vn": "normal_components", "vt": "uv"}.get(a[0])
            if key: rule = profile[key]
        elif a[0] in {"Ka", "Kd", "Ks", "Ke", "Ns", "Ni", "d", "Tr", "Pr", "Pm"}:
            rule = profile["material_numeric"]
        if rule:
            for x, y in zip(a[1:], b[1:]):
                near(float(x), float(y), rule, "export " + a[0])
        else:
            require(a == b, "export topology/material/resource identity")


def compare_exports(actual_directory):
    expected_directory = CORPUS / "fixtures/native-export"
    profile = load(CORPUS / "comparison-policy.json")["profiles"]["legacy-export-v1"]
    for record in load(expected_directory / "files.json"):
        for item in record["files"]:
            expected, actual = expected_directory / item["path"], actual_directory / item["path"]
            if expected.suffix in {".obj", ".mtl"}:
                compare_export_text(actual.read_text(), expected.read_text(), profile, expected.suffix[1:])
            else:
                require(sha(actual) == item["sha256"], "same-build exported texture bytes " + item["path"])
    print("PASS native export comparison: positions/normals/UVs/material tolerances; exact topology/bindings/texture bytes")


def verify():
    manifest = load(CORPUS / "manifest.json")
    require(manifest["canonical_profile"] == "matter-exact-json-v1", "unsupported canonical profile")
    for field in ["baseline", "fixtures"]:
        require(sha(REPO / manifest["c0"][field]) == manifest["c0"][field + "_sha256"], "changed C0 " + field)
    for item in manifest["files"]:
        path = CORPUS / item["path"]
        require(path.is_file() and sha(path) == item["sha256"], "changed/missing corpus file: " + item["path"])
    baseline = load(REPO / manifest["c0"]["fixtures"])
    require({x["id"] for x in baseline["fixtures"]} <= {x["id"] for x in manifest["samples"]}, "missing C0 fixture")
    required = set(manifest["mandatory_cases"])
    require(required == REQUIRED_CASES, "mandatory contract case list changed")
    covered = {case for sample in manifest["samples"] for case in sample["covers"]}
    require(required <= covered, "missing mandatory coverage")
    require(len({x["id"] for x in manifest["samples"]}) == len(manifest["samples"]), "duplicate fixture ID")
    source_paths = {s["path"] for s in manifest["sources"]}
    artifact_paths = {s["path"] for s in manifest["files"]}
    for sample in manifest["samples"]:
        require(sample["sources"] and sample["expected"] and sample["checks"], "unmapped sample " + sample["id"])
        for path in sample["inputs"] + sample["oracles"]:
            require((CORPUS / path).is_file(), "missing fixture mapping " + path)
            require(path in artifact_paths, "fixture mapping lacks digest " + path)
        for path in sample["sources"]:
            prefix = "tests/component-separation/"
            require(path[len(prefix):] in artifact_paths if path.startswith(prefix) else path in source_paths,
                    "source mapping lacks provenance " + path)
    # Pin provenance to repository objects, allowing later producer extraction.
    for source in manifest["sources"]:
        captured = subprocess.run(["git", "show", source["commit"] + ":" + source["path"]], cwd=REPO,
                                  check=True, stdout=subprocess.PIPE).stdout
        require(hashlib.sha256(captured).hexdigest() == source["sha256"], "source provenance " + source["path"])
    policy = load(CORPUS / "comparison-policy.json")
    oracles = load(CORPUS / "fixtures/geometry-oracles.json")
    for name in ["primitive", "extrude", "ordered_csg"]:
        compare_geometry(load(CORPUS / oracles[name]["explicit_oracle"]), oracles[name], policy["profiles"]["analytic-mesh-v1"])
    terrain_boundary(load(CORPUS / "fixtures/terrain.json"), policy["profiles"]["legacy-flat-terrain-v1"]["height_metres"])
    seeded = load(CORPUS / "fixtures/seeded-variation.json")
    vectors = {v["seed"]: v for v in load(CORPUS / "fixtures/rng-splitmix64.json")["vectors"]}
    for s in seeded["samples"]:
        require(s["first_unit"] == vectors[s["seed"]]["units"][0] and s["height_metres"] == 1+s["first_unit"]
                and s["expected_volume_metres3"] == s["height_metres"], "seeded dimension oracle")
    require(len({s["height_metres"] for s in seeded["samples"]}) == len(seeded["samples"]), "distinct seed variation")
    assembly, expected = load(CORPUS / "fixtures/repeated-assembly.asset.json"), oracles["assembly"]
    require(len(assembly["meshes"]) == expected["unique_meshes"] and len(assembly["assemblies"]) == expected["unique_assemblies"], "shared prototypes")
    children, roots = assembly["assemblies"][0]["instances"], assembly["instances"]
    require([i["id"] for i in children] == expected["child_ids"] and [i["id"] for i in roots] == expected["root_ids"], "semantic instance identities")
    require([r["id"] + "/" + c["id"] for r in roots for c in children] == expected["flattened_ids"], "flattened identity oracle")
    require(len(roots)*len(children) == expected["triangle_instances"] and len(children) == expected["delete_one_root_remaining_instances"], "assembly deletion oracle")
    material = load(CORPUS / "fixtures/material.asset.json")
    histogram = {s["material"]: s["index_count"]//3 for s in material["meshes"][0]["submeshes"]}
    require(histogram == oracles["material"]["material_triangles"], "material ownership")
    resource = material["resources"][0]
    require(resource["content"]["hex"] == sha(CORPUS / "fixtures/material-albedo.rgba8"), "texture bytes")
    require(material["textures"][0]["color_space"] == "srgb", "base-color texture color space")
    require(len(oracles["material"]["views"]) == 2 and policy["visual"]["pixel_gate"] is None, "qualified material view policy")
    for sample in load(CORPUS / "fixtures/legacy-assets.json")["samples"]:
        raw = (REPO / sample["golden_path"]).read_text().splitlines()
        fields = dict(line.split(" ", 1) for line in raw if line and not line.startswith(("material ", "map ")))
        require(all(fields[k] == v for k, v in sample["fields"].items()), "legacy golden metadata")
        for channel, expected_hash in sample["map_legacy_hashes"].items():
            require("map " + channel + " " + expected_hash in raw, "legacy texture hash")
        if "captured_geometry_path" in sample:
            lines = (CORPUS / sample["captured_geometry_path"]).read_text().splitlines()
            require(sum(line.startswith("v ") for line in lines) == int(fields["vertices"]), "native OBJ vertex count")
            require(sum(line.startswith("f ") for line in lines) == int(fields["triangles"]), "native OBJ triangle count")
            materials, active = Counter(), None
            for line in lines:
                if line.startswith("usemtl "): active = line.split()[1]
                if line.startswith("f "): materials[active] += 1
            require(dict(materials) == {m["name"]: m["triangles"] for m in sample["materials"]}, "native OBJ material ownership")
    for artifact in load(CORPUS / "evidence/historical-render.json")["artifacts"]:
        require(sha(REPO / artifact["path"]) == artifact["sha256"], "historical render reference " + artifact["path"])
    known = load(REPO / manifest["c0"]["baseline"])["known_failures_and_limits"]
    require({k["id"] for k in known} <= {k["id"] for k in manifest["known_failures_and_limits"]}, "lost known failure")
    print(f"PASS corpus: {len(manifest['samples'])} samples, {len(required)} mandatory cases, {len(manifest['files'])} pinned artifacts")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compare", choices=["primitive", "extrude", "ordered_csg"])
    parser.add_argument("--asset", type=Path)
    parser.add_argument("--exports", type=Path, help="Compare a new native-export snapshot using legacy-export-v1")
    args = parser.parse_args()
    try:
        if args.exports:
            require(args.compare is None, "choose one comparison mode")
            compare_exports(args.exports)
        elif args.compare:
            parser.error("--compare requires --asset") if args.asset is None else None
            policy = load(CORPUS / "comparison-policy.json")
            oracle = load(CORPUS / "fixtures/geometry-oracles.json")[args.compare]
            print(json.dumps(compare_geometry(load(args.asset), oracle, policy["profiles"]["analytic-mesh-v1"])))
        else:
            verify()
    except (ValueError, KeyError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"FAIL corpus: {error}\n")
