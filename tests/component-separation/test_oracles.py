"""Reject realistic output regressions using the frozen comparison policy."""
from copy import deepcopy
import importlib.util
from pathlib import Path
import pytest

root = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("corpus_verify", root / "verify.py")
verify = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verify)
policy = verify.load(root / "comparison-policy.json")
oracles = verify.load(root / "fixtures/geometry-oracles.json")


@pytest.mark.parametrize("name", ["primitive", "extrude", "ordered_csg"])
def test_analytical_output(name):
    verify.compare_geometry(verify.load(root / oracles[name]["explicit_oracle"]), oracles[name], policy["profiles"]["analytic-mesh-v1"])


@pytest.mark.parametrize("mutation", ["winding", "open", "material", "index", "bounds", "nonfinite", "submesh"])
def test_geometry_regression_rejected(mutation):
    asset = verify.load(root / oracles["primitive"]["explicit_oracle"])
    mesh = asset["meshes"][0]
    if mutation == "winding":
        for i in range(0, len(mesh["indices"]), 3):
            mesh["indices"][i+1], mesh["indices"][i+2] = mesh["indices"][i+2], mesh["indices"][i+1]
    elif mutation == "open": mesh["indices"] = mesh["indices"][:-3]
    elif mutation == "material": mesh["submeshes"][0]["material"] = "wrong"
    elif mutation == "index": mesh["indices"][0] = 999
    elif mutation == "bounds": mesh["bounds"]["min"][0] = -2
    elif mutation == "submesh": mesh["submeshes"][0]["first_index"] = -3
    else: mesh["positions"][0][0] = float("nan")
    with pytest.raises(ValueError):
        verify.compare_geometry(asset, oracles["primitive"], policy["profiles"]["analytic-mesh-v1"])


def test_cavity_occupancy_is_distinct():
    oracle = deepcopy(oracles["ordered_csg"])
    oracle["occupancy"][0]["inside"] = True
    with pytest.raises(ValueError, match="occupancy"):
        verify.compare_geometry(verify.load(root / oracle["explicit_oracle"]), oracle, policy["profiles"]["analytic-mesh-v1"])


def test_resolution_transition_boundary():
    terrain = verify.load(root / "fixtures/terrain.json")
    tolerance = policy["profiles"]["legacy-flat-terrain-v1"]["height_metres"]
    verify.terrain_boundary(terrain, tolerance)
    terrain["portable_oracle"]["fine_boundary"][1][1] += 0.01
    with pytest.raises(ValueError, match="boundary"):
        verify.terrain_boundary(terrain, tolerance)


def test_geometry_survives_vertex_renumbering():
    asset = verify.load(root / oracles["primitive"]["explicit_oracle"])
    mesh = asset["meshes"][0]
    mesh["positions"].reverse()
    mesh["indices"] = [len(mesh["positions"])-1-i for i in mesh["indices"]]
    verify.compare_geometry(asset, oracles["primitive"], policy["profiles"]["analytic-mesh-v1"])


def test_export_position_tolerance_and_material_identity():
    profile = policy["profiles"]["legacy-export-v1"]
    baseline = "v 0 0 0\nusemtl clay\nf 1/1/1 2/2/2 3/3/3\n"
    verify.compare_export_text(baseline.replace("v 0", "v 0.000009"), baseline, profile, "obj")
    with pytest.raises(ValueError, match="export v"):
        verify.compare_export_text(baseline.replace("v 0", "v 0.00002"), baseline, profile, "obj")
    with pytest.raises(ValueError, match="identity"):
        verify.compare_export_text(baseline.replace("clay", "metal"), baseline, profile, "obj")
