"""Frozen contract inputs and independent build proof; Python standard library only."""
import argparse
import hashlib
import json
from pathlib import Path
import re


def verify(build=None):
    contracts = Path(__file__).resolve().parents[1]
    repository = contracts.parents[1]
    manifest = json.loads((contracts / "tests/gate-manifest-v1.json").read_text())
    assert manifest["schema_version"] == {"major": 1, "minor": 0}
    assert manifest["abi_version"] == {"major": 1, "minor": 0}
    headers = {p.relative_to(repository).as_posix() for p in (contracts / "include").rglob("*.h")}
    assert headers == {p for p in manifest["sha256"] if p.startswith("libs/MatterContracts/include/")}
    for relative, expected in manifest["sha256"].items():
        actual = hashlib.sha256((repository / relative).read_bytes()).hexdigest()
        assert actual == expected, f"frozen contract input changed: {relative}"
    # Consumers may include public contracts and standard library headers only.
    # Legacy comparison lives in a separate target, outside this independent root.
    for source in (contracts / "tests/independent").glob("*.*"):
        if source.suffix not in (".cpp", ".h"):
            continue
        for delimiter, name in re.findall(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]', source.read_text(), re.M):
            assert (delimiter == '"' and name == "bridge.h") or (
                delimiter == "<" and (name.startswith("matter/contracts/") or "/" not in name)
            ), f"non-public consumer include: {source.name}: {name}"
    ownership = json.loads((contracts / "component-ownership.json").read_text())
    assert ownership["extraction_gate"] == manifest["task"]
    assert ownership["modules"] == manifest["scaffolds"]
    if build:
        build = Path(build)
        replies = build / ".cmake/api/v1/reply"
        index = json.loads(next(replies.glob("index-*.json")).read_text())
        model = json.loads((replies / index["reply"]["codemodel-v2"]["jsonFile"]).read_text())
        targets = model["configurations"][0]["targets"]
        by_id = {t["id"]: t["name"] for t in targets}
        expected = {"matter_contracts": set(), "gate_callee": {"matter_contracts"},
                    "gate_native": {"gate_callee", "matter_contracts"},
                    "gate_graphics": {"matter_contracts"}, "gate_c_abi": set()}
        assert set(by_id.values()) == set(expected), f"unexpected native dependency: {by_id}"
        for target in targets:
            detail = json.loads((replies / target["jsonFile"]).read_text())
            dependencies = {by_id[d["id"]] for d in detail.get("dependencies", [])}
            assert dependencies == expected[target["name"]], (target["name"], dependencies)
            for source in detail.get("sources", []):
                path = Path(source["path"])
                if not path.is_absolute():
                    path = contracts / "tests/independent" / path
                assert path.resolve().is_relative_to(contracts), f"foreign compiled source: {path}"
            for group in detail.get("compileGroups", []):
                for include in group.get("includes", []):
                    path = Path(include["path"]).resolve()
                    assert path in {contracts / "include", repository / "libs/MathLib/include"}, (
                        target["name"], "non-public include path", path)
        graph = (build / "contracts/component-graph.txt").read_text()
        assert graph == "matter_contracts|implemented|\n", "extraction scaffolds enabled in independent gate"
    print(f"PASS GATE-MANIFEST {len(manifest['sha256'])} frozen inputs; "
          "GATE-INDEPENDENT-GRAPH checked" if build else
          f"PASS GATE-MANIFEST {len(manifest['sha256'])} frozen inputs; public consumer includes")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--build")
    verify(parser.parse_args().build)
