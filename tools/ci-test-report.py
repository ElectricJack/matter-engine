"""Summarize CTest evidence, including failures, without changing its exit result."""
import argparse
import json
import math
from collections import Counter
from pathlib import Path
import xml.etree.ElementTree as ET


def summarize(junit, inventory, label=None, parallelism=1):
    if parallelism < 1:
        raise ValueError('parallelism must be positive')
    tests = {t["name"]: t for t in inventory["tests"]}
    rows = []
    for case in ET.parse(junit).getroot().iter("testcase"):
        name = case.attrib["name"]
        properties = {p['name']: p['value'] for p in tests.get(name, {}).get('properties', [])}
        seconds = float(case.get('time', '0'))
        if not math.isfinite(seconds) or seconds < 0:
            raise ValueError(f'invalid duration for {name}: {seconds}')
        rows.append({"name": name, "seconds": seconds,
                     "failed": case.find("failure") is not None or case.find("error") is not None,
                     "skipped": case.find("skipped") is not None,
                     "labels": properties.get('LABELS', []),
                     "resource_locks": properties.get('RESOURCE_LOCK', [])})
    rows.sort(key=lambda t: (-t["seconds"], t["name"]))
    counts = Counter(r['name'] for r in rows)
    expected = {name for name, test in tests.items() if label is None or any(
        p['name'] == 'LABELS' and label in p['value'] for p in test.get('properties', []))}
    locks = Counter()
    for row in rows:
        for lock in row['resource_locks']:
            locks[lock] += row['seconds']
    test_seconds = sum(r['seconds'] for r in rows)
    # Lower bound for these measured fixtures on this platform. It is not a
    # prediction for smaller inputs or a faster runner.
    floor = max([test_seconds / parallelism] + [r['seconds'] for r in rows] + list(locks.values()))
    return {"tests": len(rows), "failures": sum(r["failed"] for r in rows),
            "skipped": sum(r["skipped"] for r in rows),
            "slowest_20": rows[:20], "over_60_seconds": [r for r in rows if r["seconds"] > 60],
            'all_tests': rows, 'summed_test_seconds': test_seconds,
            'parallelism': parallelism, 'measured_parallel_floor_seconds': floor,
            'resource_lock_seconds': dict(locks), 'selected_label': label,
            'expected_tests': len(expected), 'missing_tests': sorted(expected - counts.keys()),
            'unexpected_tests': sorted(counts.keys() - expected),
            'duplicate_tests': sorted(name for name, count in counts.items() if count != 1)}


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--junit", required=True, type=Path)
    p.add_argument("--inventory", required=True, type=Path)
    p.add_argument("--output", required=True, type=Path)
    p.add_argument('--label')
    p.add_argument('--parallelism', type=int, default=1)
    args = p.parse_args()
    result = summarize(args.junit, json.loads(args.inventory.read_text(encoding="utf-8-sig")),
                       args.label, args.parallelism)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    for row in result["slowest_20"]:
        print(f'{row["seconds"]:9.3f}s {row["name"]} {"FAIL" if row["failed"] else ""}')
    if args.label and (not result['tests'] or result['missing_tests'] or result['unexpected_tests']
                       or result['duplicate_tests'] or result['skipped']):
        raise SystemExit('Selected CTest inventory was not fully executed; see timing report.')


if __name__ == "__main__":
    main()
