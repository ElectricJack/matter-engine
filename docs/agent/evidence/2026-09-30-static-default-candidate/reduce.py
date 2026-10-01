#!/usr/bin/env python3
"""Validate and reduce four serial native candidate captures."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re

parser = argparse.ArgumentParser()
parser.add_argument('--root', default='/mnt/c/tmp/quick-meadow68-4-20260930')
args = parser.parse_args()
root = Path(args.root)
repo = Path(__file__).resolve().parents[4]
previous = repo / 'docs/agent/evidence/2026-09-30-hdgeo-performance/reduce_captures.py'
spec = importlib.util.spec_from_file_location('previous', previous)
helpers = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helpers)
protocol = json.loads((root / 'protocol.json').read_text())
runs = []
for recorded in protocol['runs']:
    assert recorded['exit_code'] == 0 and recorded['screenshot_captured']
    setup, warmup = recorded['setup'], recorded['warmup_seconds']
    folder = root / f'{setup}-w{warmup}'
    run = helpers.reduce_run(folder / 'pom_off.json', setup, warmup, 1)
    assert (folder / 'git_sha.txt').read_text().strip() == protocol['source_sha']
    assert (folder / 'editor_sha256.txt').read_text().split()[0] == protocol['editor_sha256']
    log = (folder / 'pom_off.log').read_text(errors='replace')
    run['scene_census_lines'] = [line for line in log.splitlines()
                               if line.startswith(('STATS,candidate-view,', 'STATSVT,candidate-view,'))]
    assert len(run['scene_census_lines']) == 2
    values = run['scene_census_lines'][0].split(',')
    run['scene_at_screenshot_request'] = dict(instances_active=int(values[6]),
                                            raster_batches=int(values[7]),
                                            raster_triangles=int(values[8]))
    trace = json.loads((folder / 'pom_off.trace.json').read_text())['traceEvents']
    run['geometry_trace_names'] = sorted({e['name'] for e in trace
                                         if e['name'].startswith('geometry.')})
    run['paging_diagnostic_lines'] = sum(bool(re.search(r'\bpaging[_ ]', line))
                                         for line in log.splitlines())
    if setup == 'static':
        assert not run['geometry_trace_names'], run['geometry_trace_names']
        assert run['paging_diagnostic_lines'] == 0
    else:
        assert 'geometry.update' in run['geometry_trace_names']
        assert run['last_geometry_coverage'] is not None
    run['screenshot'] = dict(path=str(folder / 'view.png'),
        sha256=hashlib.sha256((folder / 'view.png').read_bytes()).hexdigest(),
        delay_from_warmup_marker_seconds=recorded['shot_delay_from_warmup_marker_seconds'])
    runs.append(run)
assert {(r['setup'], r['warmup_seconds']) for r in runs} == {
    (s, w) for s in ('static', 'vg') for w in (45, 300)}
summary = dict(protocol=protocol, authored_detailed_rock_placements=3,
               authored_detailed_rock_assets=3, cube_resolution=128,
               placed_source_triangles=589824, runs=runs)
(root / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
print('| path / warmup | frames | cadence median / p99 / max ms | >100 ms | >1 s | GPU median / p99 / max ms |')
print('|---|---:|---|---:|---:|---|')
for r in runs:
    def cell(d):
        return ' / '.join(f'{d[k]:.2f}' for k in ('median_ms', 'p99_ms', 'max_ms'))
    print(f"| {r['setup']} / {r['warmup_seconds']}s | {r['frames']} | {cell(r['interval'])} | "
          f"{r['over_100_ms']} | {r['over_1_s']} | {cell(r['gpu_pass_statistics']['total'])} |")
