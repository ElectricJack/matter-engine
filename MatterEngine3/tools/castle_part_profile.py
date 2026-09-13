#!/usr/bin/env python3
"""Run native castle fixtures serially and retain JSONL, stderr and SHA256 provenance.

Use native Python with native Windows paths, or WSL Python with --native-repo
and --native-output to supply paths understood by the native executable.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import statistics
import subprocess
import time

FIXTURES = ['stone0', 'stone1', 'beam_short', 'beam_long', 'plank', 'slab']


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def summarize(rows):
    groups = {}
    for row in rows:
        if row.get('type') == 'sample':
            groups.setdefault(row['mode'], []).append(row)
    result = {}
    for mode, samples in groups.items():
        result[mode] = {'n': len(samples)}
        for key in ['install_ms', 'decode_probe_ms', 'stage_ms', 'commit_ms']:
            values = sorted(row[key] for row in samples)
            result[mode][key] = dict(p50=statistics.median(values),
                p95_nearest_rank=values[math.ceil(.95 * len(values))-1],
                minimum=values[0], maximum=values[-1])
        result[mode]['triangles'] = sorted(set(row['serialized_triangles'] for row in samples))
        result[mode]['fallback_lods'] = sorted(set(row['published_lods'] for row in samples))
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--exe', required=True, type=Path)
    ap.add_argument('--repo', required=True, type=Path)
    ap.add_argument('--output', required=True, type=Path)
    ap.add_argument('--native-repo')
    ap.add_argument('--native-output')
    ap.add_argument('--samples', type=int, default=3)
    ap.add_argument('--fixtures', nargs='+', choices=FIXTURES, default=FIXTURES)
    ap.add_argument('--flatten', action='store_true')
    ap.add_argument('--timeout', type=int, default=900)
    args = ap.parse_args()
    if not 1 <= args.samples <= 100:
        ap.error('--samples must be 1..100')
    args.output.mkdir(parents=True, exist_ok=False)
    repo = args.repo.resolve()
    sources = ['MatterEngine3/src/script_host.cpp', 'MatterEngine3/src/part_graph.cpp',
               'MatterEngine3/src/part_bundle.h', 'MatterEngine3/src/render/part_store.cpp',
               'MatterEngine3/src/part_flatten.cpp', 'MatterEngine3/src/lod_bake.cpp',
               'MatterEngine3/tests/castle_part_bench.cpp', 'libs/MatterSurfaceLib/src/surface.c']
    sources += [str(p.relative_to(repo)) for p in (repo/'projects/world_demo').glob('**/castle*.js')]
    sources += [str(p.relative_to(repo)) for p in (repo/'projects/world_demo/objects').glob('Castle*.js')]
    provenance = dict(exe=str(args.exe.resolve()), exe_sha256=sha(args.exe),
        source_sha256={p: sha(repo/p) for p in sources if (repo/p).exists()},
        note='Source hashes describe the runner input tree; externally frozen binaries require their own build-source manifest.',
        started_utc=time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), samples=args.samples,
        timing='One cold-service miss + samples-1 warm-service misses per process; every miss followed by disk-cache hit. stage_load is the compositional CPU fallback, with OS reads warmed by a separate decode probe. No GPU upload.',
        env={key: value for key, value in os.environ.items() if key in ['MATTER_SDF_GPU', 'MATTER_MAX_LOD_LEVELS', 'MATTER_SDF_NORMALS_LEGACY']})
    (args.output/'provenance.json').write_text(json.dumps(provenance, indent=2))
    summary = {}
    for fixture in args.fixtures:
        native_out = (args.native_output or str(args.output.resolve())).rstrip('/\\') + '/' + fixture
        command = [str(args.exe.resolve()), args.native_repo or str(repo), native_out, fixture, str(args.samples)]
        if args.flatten:
            command.append('--flatten')
        started = time.perf_counter()
        with (args.output/f'{fixture}.jsonl').open('w') as out, (args.output/f'{fixture}.log').open('w') as err:
            process = subprocess.run(command, stdout=out, stderr=err, timeout=args.timeout)
        if process.returncode:
            raise SystemExit(f'{fixture} failed ({process.returncode}); see retained stderr')
        rows = [json.loads(line) for line in (args.output/f'{fixture}.jsonl').read_text().splitlines() if line.startswith('{')]
        samples = [row for row in rows if row.get('type') == 'sample']
        if len(samples) != 2 * args.samples:
            raise SystemExit(f'{fixture}: missing sample rows')
        summary[fixture] = dict(process_wall_seconds=time.perf_counter()-started, measurements=summarize(rows))
        (args.output/'summary.json').write_text(json.dumps(summary, indent=2))
        print(f'{fixture}: {len(samples)} samples retained', flush=True)


if __name__ == '__main__':
    main()
