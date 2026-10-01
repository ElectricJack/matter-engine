#!/usr/bin/env python3
"""Reduce the preserved 18 HD geometry captures; no new measurements inferred."""
import argparse
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import re
import statistics

ROOT = Path('/mnt/c/tmp/clear-ridge10-20260930')
spec = importlib.util.spec_from_file_location('attribution', Path.cwd() / 'tools/frame_attribution.py')
attribution = importlib.util.module_from_spec(spec)
spec.loader.exec_module(attribution)
ZONES = ['geometry.update', 'geometry.scene_completion', 'geometry.scene_assembly',
         'rt.rung_select', 'pf.static', 'publish.vulkan', 'publish', 'vt.record',
         'geometry.scene_reused', 'geometry.cut_reused', 'geometry.cut_updated']
COUNTERS = ['instances.rt_scanned', 'instances.sector_lod_scanned',
            'geometry.scene_reused', 'geometry.cut_reused', 'geometry.cut_updated']

def distribution(values):
    ordered = sorted(values)
    if not ordered:
        return None
    return dict(samples=len(ordered), median_ms=statistics.median(ordered),
                mean_ms=statistics.mean(ordered),
                p95_ms=ordered[max(0, math.ceil(.95 * len(ordered)) - 1)],
                p99_ms=ordered[max(0, math.ceil(.99 * len(ordered)) - 1)],
                max_ms=ordered[-1])

def checksum(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def trace_window(path, count):
    frames, pending, registered = [], {}, set()
    for event in json.loads(path.read_text())['traceEvents']:
        if event['ph'] == 'X':
            name = event['name']
            registered.add(name)
            pending[name] = pending.get(name, 0.) + event['dur'] / 1000.
        elif event['ph'] == 'C' and event['name'] == 'frame_ms':
            frames.append(dict(wall_ms=event['args']['ms'], zones=pending, counters={}))
            pending = {}
        elif event['ph'] == 'C' and frames:
            frames[-1]['counters'][event['name']] = event['args'].get('n', 0)
    frames = frames[-count:]
    zones = {z: distribution([f['zones'].get(z, 0.) for f in frames])
             for z in ZONES if z in registered}
    counters = {c: dict(mean=statistics.mean([f['counters'].get(c, 0) for f in frames]),
                        total=sum(f['counters'].get(c, 0) for f in frames),
                        nonzero_frames=sum(f['counters'].get(c, 0) != 0 for f in frames))
                for c in COUNTERS if any(c in f['counters'] for f in frames)}
    return dict(samples=len(frames), zones=zones, counters=counters)

def reduce_run(path, setup, warmup, repeat):
    data = json.loads(path.read_text())
    log_path = path.with_suffix('.log')
    log = log_path.read_text(errors='replace')
    # Check explicit completion and original runner settings before reducing.
    assert data['world'] == 'StreamMountain'
    assert data['pom_enabled'] is False
    assert data['raster_width'] == 1920 and data['raster_height'] == 1080
    assert data['vk_rt_effective'] is True and data['validation_errors'] == 0
    assert data.get('gbuffer_profile_mode', 'full') == 'full'
    # stdout/stderr diagnostic writers can interleave inside a printf line.
    # Preserve their raw logs; validate phase and duration tokens separately,
    # alongside the retained launch recipe, rather than altering a capture.
    warmup_token = re.search(r'warming for (\d+(?:\.\d+)?)', log)
    sample_token = re.search(r'perf: sampling for (\d+(?:\.\d+)?)', log)
    assert warmup_token and float(warmup_token[1]) == warmup
    assert sample_token and float(sample_token[1]) == 20 and 'perf: wrote ' in log
    assert len(data['frame_times_ms']) == data['frames']
    frames = data['frame_times_ms']
    hist = []
    edges = (0.,) + attribution.HITCH_EDGES_MS + (None,)
    for lo, hi in zip(edges, edges[1:]):
        hist.append(dict(lower_exclusive_ms=lo, upper_inclusive_ms=hi,
                         count=sum((lo == 0 or x > lo) and (hi is None or x <= hi) for x in frames)))
    artifacts = {}
    for suffix in ('.json', '.trace.json', '.log', '.commands.txt', '.gpu_before.txt',
                   '.gpu_during.csv', '.wall.txt'):
        artifact = path.with_suffix(suffix)
        assert artifact.is_file(), artifact
        artifacts[artifact.name] = dict(path=str(artifact), sha256=checksum(artifact))
    telemetry = []
    for keyword in ('paging_coverage ', 'paging_memory ', 'paging_gpu ', 'paging_scene ',
                    'paging_cut ', 'paging roots', 'gpu_mb=', 'vram ', 'vk_memory ', 'STATIC_CAP'):
        matching = [line for line in log.splitlines() if keyword in line]
        if matching:
            telemetry.append(matching[-1])
    settings = {k: data[k] for k in ['world', 'pom_enabled', 'raster_width', 'raster_height',
                                    'vk_rt_effective', 'rt_enabled', 'rt_samples',
                                    'validation_errors', 'selected_dlss_mode', 'active_dlss_mode']}
    coverage_lines = [line for line in log.splitlines() if 'paging_coverage ' in line]
    coverage = dict((k, int(v)) for k, v in re.findall(r'(\w+)=(\d+)', coverage_lines[-1])) if coverage_lines else None
    memory_lines = [line for line in log.splitlines() if '[geometry] vram ' in line]
    memory_peaks = {}
    for line in memory_lines:
        for key, value in re.findall(r'(\w+)=(\d+)(?![\d/])', line):
            memory_peaks[key] = max(memory_peaks.get(key, 0), int(value))
    cache_outcomes = {outcome: len(re.findall(r'terrain_cache .*?outcome='+outcome+r'\b', log))
                      for outcome in ['hit', 'cold', 'missing', 'miss', 'failed']}
    return dict(setup=setup, warmup_seconds=warmup, repeat=repeat, perf_path=str(path),
                frames=data['frames'], frame_times_ms=frames, interval=distribution(frames),
                over_100_ms=sum(x > 100 for x in frames), over_1_s=sum(x > 1000 for x in frames),
                histogram=hist, gpu_pass_statistics=data['gpu_pass_statistics']['passes'],
                peak_whole_gpu_vram_mib=attribution._peak_vram_mib(path),
                static_vertex_upload_delta=data['static_vertex_upload_delta'],
                static_cluster_upload_delta=data['static_cluster_upload_delta'],
                stable_instance_upload_delta=data['stable_instance_upload_delta'],
                loop_render_ms=data['loop_render_ms'],
                loop_render_metric='latest_ema_at_sample_end',
                elapsed_seconds=float(path.with_suffix('.wall.txt').read_text().split()[1]),
                settings=settings, telemetry_last_lines=telemetry, artifacts=artifacts,
                last_geometry_coverage=coverage, memory_consumer_peak_values=memory_peaks,
                logged_terrain_cache_outcomes=cache_outcomes,
                trace=trace_window(path.with_suffix('.trace.json'), data['frames']))

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--partial', action='store_true')
    args = parser.parse_args()
    runs = []
    for warmup in (45,300):
        for setup in ('historical-baseline','static','vg'):
            folder = Path(f'/mnt/c/tmp/clear-ridge-1-w{warmup}') if setup == 'historical-baseline' else ROOT/f'{setup}-w{warmup}'
            for repeat in (1,2,3):
                path = folder/f'pom_off_r{repeat}.json'
                if args.partial and not path.with_suffix('.wall.txt').exists():
                    continue
                runs.append(reduce_run(path, setup, warmup, repeat))
    print('Verified captures:',len(runs))
    (ROOT/'summary.json').write_text(json.dumps(dict(schema_version=1, protocol=json.loads((ROOT/'protocol.json').read_text()),
                                                  runs=runs), indent=2)+'\n')
    for warmup in (45,300):
        for setup in ('historical-baseline','static','vg'):
            group=[r for r in runs if r['warmup_seconds']==warmup and r['setup']==setup]
            if not group:
                continue
            paths=[r['perf_path'] for r in group]
            (ROOT/f'{setup}-w{warmup}-hitches.md').write_text(attribution.render_hitches(paths))
            (ROOT/f'{setup}-w{warmup}-attribution.md').write_text(attribution.render(paths))
            for r in group:
                gpu=r['gpu_pass_statistics']['total']
                print(setup,warmup,r['repeat'],r['frames'],
                      'GPU',*[round(gpu[k],2) for k in ('median_ms','p99_ms','max_ms')],
                      'hitches',r['over_100_ms'],r['over_1_s'],'VRAM',r['peak_whole_gpu_vram_mib'])

if __name__ == '__main__':
    main()
