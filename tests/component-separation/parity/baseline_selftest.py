#!/usr/bin/env python3
"""Proves run_parity.py cannot accept a regression through its baseline.

Feeds run_parity.judge_baseline the pinned baseline and a real candidate from
a previous run (--out of run_parity.py), then tampered copies of each. The
pinned pair must be accepted, as must a geometry drift inside the profile
tolerance. Every other tamper must be rejected. Nothing is written in the
repository; tampered baselines live in a temporary directory.

Usage: python3 baseline_selftest.py PARITY_OUT_DIR   (exit 0 = every check held)
"""
import copy
import hashlib
import json
from pathlib import Path
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_parity as rp  # noqa: E402


def main():
    if len(sys.argv) != 2:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    candidate = json.loads((Path(sys.argv[1]) / "candidate-baseline.json").read_text(encoding="utf-8"))
    corpus, pinned = rp.Corpus(), rp.BASELINE.read_bytes()
    pin = hashlib.sha256(pinned).hexdigest()
    if pin != rp.BASELINE_SHA256:
        print("baseline.json does not match BASELINE_SHA256; nothing to self-test against", file=sys.stderr)
        return 1
    results = []

    with tempfile.TemporaryDirectory() as work:
        work = Path(work)

        def judge(base_bytes, cand, sha):
            path = work / "baseline.json"
            path.write_bytes(base_bytes)
            saved = rp.BASELINE, rp.BASELINE_SHA256, rp.REPO
            rp.BASELINE, rp.BASELINE_SHA256, rp.REPO = path, sha, work
            try:
                return rp.judge_baseline(corpus, cand, None)
            finally:
                rp.BASELINE, rp.BASELINE_SHA256, rp.REPO = saved

        def check(name, status, accept):
            accepted = not status["problems"]
            results.append(accepted == accept)
            print(f"{'ok ' if accepted == accept else 'BAD'} {name}: {'accepted' if accepted else 'rejected'}"
                  f"{'' if accepted else '; ' + status['problems'][0]}")

        def with_wire(mutate):
            cand = copy.deepcopy(candidate)
            line = next(l for l in cand["geometry"]["G-PRIMITIVE"]["native"] if "wire" in l)
            value = json.loads(cand["wires"][line["wire"]["sha256"]])
            mutate(value["asset"]["meshes"][0])
            text = json.dumps(value, separators=(",", ":"))
            digest = hashlib.sha256(text.encode()).hexdigest()
            cand["wires"][digest] = text
            line["wire"] = {"sha256": digest, "metadata": rp.metadata_digest(value)}
            return cand

        def nudge(delta):
            def mutate(mesh):
                mesh["positions"][0][0] += delta
            return mutate

        def rename_material(mesh):
            mesh["submeshes"][0]["material"] = "other"

        def drop_triangle(mesh):
            mesh["indices"] = mesh["indices"][:-3]

        def reserialised(value):
            return json.dumps(value, indent=1, ensure_ascii=False).encode() + b"\n"

        check("pinned baseline, identical candidate", judge(pinned, candidate, pin), True)
        edited = json.loads(pinned)
        edited["authoring"]["A-PRIMITIVE"]["status"] = "regressed"
        check("baseline overwritten without re-pin", judge(reserialised(edited), candidate, pin), False)
        check("pin removed", judge(pinned, candidate, "unpinned"), False)
        check("position drift 1e-3 m (outside 1e-6)", judge(pinned, with_wire(nudge(1e-3)), pin), False)
        check("position drift 4e-7 m (inside 1e-6)", judge(pinned, with_wire(nudge(4e-7)), pin), True)
        check("submesh material renamed", judge(pinned, with_wire(rename_material), pin), False)
        check("triangle dropped", judge(pinned, with_wire(drop_triangle), pin), False)
        cand = copy.deepcopy(candidate)
        next(l for l in cand["geometry"]["G-MALFORMED"]["native"] if l.get("status", 0) != 0)["status"] = 0
        check("malformed recipe now succeeds", judge(pinned, cand, pin), False)
        cand = copy.deepcopy(candidate)
        cand["runtime"]["R-OWNERSHIP"]["kept_survives_dispose"] = False
        check("kept result no longer survives dispose", judge(pinned, cand, pin), False)
        cand = copy.deepcopy(candidate)
        cand["inputs"][next(iter(cand["inputs"]))] = "0" * 64
        check("input fixture changed", judge(pinned, cand, pin), False)
        corrupt = json.loads(pinned)
        key = next(iter(corrupt["wires"]))
        corrupt["wires"][key] = corrupt["wires"][key].replace("1", "2", 1)
        repinned = reserialised(corrupt)
        check("stored wire edited and file re-pinned", judge(repinned, candidate, hashlib.sha256(repinned).hexdigest()),
              False)

    print("SELFTEST PASS" if all(results) else "SELFTEST FAIL")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
