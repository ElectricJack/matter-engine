"""Summarize CTest evidence, including failures, without changing its exit result."""
import argparse
import json
from pathlib import Path
import xml.etree.ElementTree as ET


def summarize(junit, inventory):
    tests = {t["name"]: t for t in inventory["tests"]}
    rows = []
    for case in ET.parse(junit).getroot().iter("testcase"):
        name = case.attrib["name"]
        labels = next((p["value"] for p in tests.get(name, {}).get("properties", [])
                       if p["name"] == "LABELS"), [])
        rows.append({"name": name, "seconds": float(case.get("time", "0")),
                     "failed": case.find("failure") is not None or case.find("error") is not None,
                     "skipped": case.find("skipped") is not None,
                     "labels": labels})
    rows.sort(key=lambda t: (-t["seconds"], t["name"]))
    return {"tests": len(rows), "failures": sum(r["failed"] for r in rows),
            "skipped": sum(r["skipped"] for r in rows),
            "slowest_20": rows[:20], "over_60_seconds": [r for r in rows if r["seconds"] > 60]}


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--junit", required=True, type=Path)
    p.add_argument("--inventory", required=True, type=Path)
    p.add_argument("--output", required=True, type=Path)
    args = p.parse_args()
    result = summarize(args.junit, json.loads(args.inventory.read_text(encoding="utf-8-sig")))
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    for row in result["slowest_20"]:
        print(f'{row["seconds"]:9.3f}s {row["name"]} {"FAIL" if row["failed"] else ""}')


if __name__ == "__main__":
    main()
