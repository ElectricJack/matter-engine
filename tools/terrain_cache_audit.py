"""Native Windows terrain cook/reopen audit; run with py -3 from the repo.

Preparation displays source receivers while cooking the normal geometry cache.
Reopen forbids geometry compilation; misses invalidate the result even if source
fallback remains visible. OS file caching is deliberately not flushed.
"""
import argparse
import collections
import json
import os
from pathlib import Path
import re
import subprocess
import threading
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=['prepare', 'reopen', 'load'])
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--prepared', type=Path)
    parser.add_argument('--prepared-sectors', action='store_true', help='Require prepared-sector hits on reopen/load')
    parser.add_argument('--timeout', type=float, default=1800)
    parser.add_argument('--read-ahead-mb', type=int, choices=[0,1,4,8,16], default=4)
    parser.add_argument('--sector-readers', type=int, choices=[1,2,4,8], default=1)
    parser.add_argument('--sector-read-ahead-mb', type=int, choices=[0,1,4,8,16], default=0)
    parser.add_argument('--geometry-inflight', type=int, choices=[128,256,512,1024,2048,4096], default=128)
    parser.add_argument('--geometry-read-batch', type=int, choices=[32,64,128,256,512,1024], default=32)
    parser.add_argument('--geometry-gpu-mb', type=int, default=3072,
                        help='Geometry page GPU reservation budget (default: 3072 MiB)')
    parser.add_argument('--bounded-paging', action='store_true',
                        help='For load, require all in-view roots ready; allow offscreen source fallback')
    parser.add_argument('--turn-after-ready', help='Second camera ex,ey,ez,tx,ty,tz after first stable readiness')
    parser.add_argument('--identity-cache', action='store_true')
    parser.add_argument('--identity-cook', action='store_true')
    parser.add_argument('--vt-cache', type=Path, help='Finished GPU-format VT page store (load mode)')
    parser.add_argument('--vt-cook', action='store_true', help='Capture missing VT pages and wait for durable writes')
    args = parser.parse_args()
    if not 1 <= args.geometry_gpu_mb <= 4096:
        parser.error('--geometry-gpu-mb must be 1..4096 MiB')
    if args.bounded_paging and args.mode != 'load':
        parser.error('--bounded-paging requires load mode')
    if (args.vt_cache and args.mode != 'load') or (args.vt_cook and not args.vt_cache):
        parser.error('--vt-cache requires load mode; --vt-cook requires --vt-cache')
    if args.identity_cook and not args.identity_cache:
        parser.error("--identity-cook requires --identity-cache")
    turn_camera = None
    if args.turn_after_ready:
        try:
            turn_camera = [float(v) for v in args.turn_after_ready.split(',')]
            if len(turn_camera) != 6 or args.mode != 'load': raise ValueError()
        except ValueError:
            parser.error('--turn-after-ready requires six comma-separated floats and load mode')
    repo = Path(__file__).resolve().parent.parent
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    fifo = out / 'commands.txt'
    fifo.write_text('render_path raster\nset render.gi.enabled false\nset render.pom.enabled false\nset render.cloud_shadows.enabled false\n')
    env = os.environ.copy()
    env.update({
        'MATTER_WORLD': 'StreamMountain', 'MATTER_GEOMETRY_TERRAIN': '1',
        'MATTER_VOLUMETRIC_SECTORS': '1',
        'MATTER_GEOMETRY_RASTER_ONLY': '1', 'MATTER_DISABLE_VK_RT': '1',
        'MATTER_VOLUMETRICS': '0', 'MATTER_PRESENT_MODE': 'immediate',
        'MATTER_VT_INDIRECTION_MB': '128',
        'MATTER_VK_CPU_RESERVE_VERTEX_MB': '2048', 'MATTER_VK_CPU_RESERVE_INDEX_MB': '256',
        'MATTER_VK_STATIC_RESERVE_VERTEX_MB': '2048',
        'MATTER_GEOMETRY_PAGES': '1', 'MATTER_GEOMETRY_MODULE': 'MountainDetailRock',
        'MATTER_GEOMETRY_MIN_TRIANGLES': '16384', 'MATTER_GEOMETRY_CPU_MB': '1024',
        'MATTER_GEOMETRY_ROOT_MB': '1024', 'MATTER_GEOMETRY_GPU_MB': str(args.geometry_gpu_mb),
        'MATTER_GEOMETRY_READ_AHEAD_MB': str(args.read_ahead_mb), 'MATTER_GEOMETRY_UPLOAD_CPU_MS': '4',
        'MATTER_GEOMETRY_MAX_INFLIGHT': str(args.geometry_inflight),
        'MATTER_GEOMETRY_READ_BATCH': str(args.geometry_read_batch),
        'MATTER_PREPARED_SECTOR_READERS': str(args.sector_readers),
        'MATTER_PREPARED_SECTOR_READ_AHEAD_MB': str(args.sector_read_ahead_mb),
        'MATTER_GEOMETRY_PAGES_PROFILE': '1', 'MATTER_TERRAIN_CACHE_AUDIT': '1',
        'MATTER_STREAM_FILL_PROFILE': '1',
        'MATTER_CAM': '425,25,1465,419,23,1455',
        'MATTER_WINDOW_WIDTH': '1280', 'MATTER_WINDOW_HEIGHT': '720',
        'MATTER_HIDE_UI': '1', 'MATTER_CMD_FIFO': str(fifo),
        'MATTER_PROFILE_TRACE': str(out / 'profile.json'),
        'MATTER_VT_TRACE': str(out / 'vt.jsonl'),
        'MATTER_VT_PREPARATION_PROFILE': '1',
        'MATTER_PREPARED_IDENTITY_CACHE': '1' if args.identity_cache else '0',
        'MATTER_PREPARED_IDENTITY_COOK': '1' if args.identity_cook else '0',
        'MATTER_GEOMETRY_CACHE_ONLY': '0' if args.mode == 'prepare' else '1',
        'MATTER_PREPARED_SECTOR_CACHE_ONLY': '1' if args.prepared_sectors and args.mode != 'prepare' else '0',
        'MATTER_GEOMETRY_PREPARE_ONLY': '0' if args.mode == 'load' else '1',
    })
    for key in ['MATTER_VT_ENCODED_CACHE', 'MATTER_VT_ENCODED_COOK']:
        env.pop(key, None)
    if args.vt_cache:
        env['MATTER_VT_ENCODED_CACHE'] = str(args.vt_cache.resolve())
        env['MATTER_VT_ENCODED_COOK'] = '1' if args.vt_cook else '0'
    for key in ['MATTER_PERF_OUTPUT', 'MATTER_PERF_WARMUP_SECONDS', 'MATTER_PERF_SAMPLE_SECONDS']:
        env.pop(key, None)
    (out / 'environment.json').write_text(json.dumps({k:v for k,v in env.items() if k.startswith('MATTER_')}, indent=2))
    start = time.monotonic()
    stop_sent = threading.Event()
    complete = False
    streamer_idle_seconds = None
    stable_since = None
    previous = None
    last_cache = start
    events = []
    prepared_events = []
    identity_sources = collections.Counter()
    prepared_surface_reuse = collections.Counter()
    prepared_writes = []
    statuses = []
    failures = []
    page_inflight = None
    geometry_coverage = None
    visible_coverage = None
    vt_ready = False
    last_vt_poll = 0
    vt_samples = []
    encoded_samples = []
    encoded_ready = not args.vt_cache
    readiness_phases = []
    visible_sector_samples = []
    visible_detail_samples = []
    turn_seconds = None
    def quit_run():
        if not stop_sent.is_set():
            stop_sent.set()
            with fifo.open('a') as f:
                f.write(f'stats cache_audit_end\nshot_now {(out / "terrain.png").as_posix()}\nquit\n')
    deadline = threading.Timer(args.timeout, quit_run)
    deadline.daemon = True
    deadline.start()
    try:
        with (out / 'controller.log').open('w', encoding='utf8') as log:
            proc = subprocess.Popen([str(repo / 'MatterEditor/build/windows-msvc/editor.exe')],
                cwd=repo / 'MatterEditor', env=env, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, text=True, encoding='utf8', errors='replace')
            for line in proc.stdout:
                log.write(line)
                log.flush()
                now = time.monotonic()
                surface_reuse = re.search(r'prepared_surface hash=\w+ reuse=(\d+)', line)
                if surface_reuse:
                    prepared_surface_reuse['reused' if surface_reuse.group(1) == '1' else 'recomputed'] += 1
                identity = re.search(r'prepared_identity hash=\w+ ms=[\d.]+ source=(\w+)', line)
                if identity: identity_sources[identity[1]] += 1
                if 'prepared identity commit failed:' in line: failures.append(line.strip())
                if 'paging failed:' in line or 'validation error' in line.lower():
                    failures.append(line.strip())
                if 'prepared sector cache miss; generation forbidden' in line:
                    failures.append(line.strip())
                    quit_run()
                match = re.search(r'prepared_sector hash=(\w+) outcome=(\w+) ms=([\d.]+) reason=(.*)', line)
                if match:
                    last_cache = now
                    prepared_events.append(dict(seconds=now-start, hash=match[1], outcome=match[2], ms=float(match[3]), reason=match[4].strip()))
                match = re.search(r'prepared_sector_write hash=(\w+) saved=(\d+) reason=(.*)', line)
                if match:
                    prepared_writes.append(dict(hash=match[1], saved=int(match[2]), reason=match[3].strip()))
                match = re.search(r'terrain_cache key=(\S+) outcome=(\S+) compiled=(\d+) lookup_ms=([\d.]+) compile_ms=([\d.]+) write_ms=([\d.]+) reason=(.*)', line)
                if match:
                    last_cache = now
                    events.append(dict(seconds=now-start, key=match[1], outcome=match[2], compiled=int(match[3]),
                        lookup_ms=float(match[4]), compile_ms=float(match[5]), write_ms=float(match[6]), reason=match[7].strip()))
                match = re.search(r'pages assets=\d+ pages=\d+ inflight=(\d+)', line)
                if match:
                    page_inflight = int(match[1])
                coverage = re.search(r'paging_coverage rejected_assets=(\d+) unready_assets=(\d+) source_fallbacks=(\d+)', line)
                if coverage:
                    geometry_coverage = tuple(map(int, coverage.groups()))
                visible = re.search(r'visible_assets=(\d+) visible_unready=(\d+) visible_roots=(\d+) visible_ready_roots=(\d+)', line)
                if visible:
                    visible_coverage = tuple(map(int, visible.groups()))
                if line.startswith('STATSVT,cache_audit_poll,'):
                    values = dict(re.findall(r'(active|rejected|queue)=(\d+)', line))
                    vt_ready = values.get('active') == '1' and values.get('rejected') == '0' and values.get('queue') == '0'
                    vt_samples.append(dict(seconds=now-start, ready=vt_ready, values=values))
                if '[vt-cache]' in line and 'hits=' in line:
                    values = {k:int(v) for k,v in re.findall(r'(\w+)=(\d+)', line)}
                    encoded_samples.append(dict(seconds=now-start, **values))
                    encoded_ready = (values.get('pending_pages') == 0 and values.get('errors') == 0 and
                        values.get('rejected') == 0 and values.get('hits',0)+values.get('persisted',0) > 0)
                detail = re.search(r'visible_detail cut_generation=(\d+) requests=(\d+) request_overflow=(\d+) refinement_fallback=(\d+)', line)
                if detail:
                    generation, requests, overflow, fallback = map(int, detail.groups())
                    identity = re.search(r'frame=(\d+) view=(\d+) current_view=(\d+) current_cut=(\d+)', line)
                    sample = dict(seconds=now-start, cut_generation=generation, requests=requests,
                                  request_overflow=bool(overflow), refinement_fallback=bool(fallback), snapshot_matches=False)
                    if identity:
                        frame_id, view, current_view, current_cut = map(int, identity.groups())
                        sample.update(frame=frame_id, view=view, current_view=current_view, current_cut=current_cut,
                                      snapshot_matches=bool(frame_id and view and view==current_view and generation==current_cut))
                    visible_detail_samples.append(sample)
                visible = re.search(r'CACHE_VISIBLE_SECTORS revision=(\d+) valid=(\d+) desired=(\d+) pending=(\d+)', line)
                if visible:
                    revision, valid, desired, pending = map(int, visible.groups())
                    visible_sector_samples.append(dict(seconds=now-start, revision=revision, valid=bool(valid), desired=desired, pending=pending))
                match = re.search(r'CACHE_AUDIT state=(\d+) resident=(\d+) inflight=(\d+) bake_ready=(\d+)', line)
                if match:
                    state, resident, inflight, ready = map(int, match.groups())
                    statuses.append(dict(seconds=now-start, state=state, resident=resident, inflight=inflight, ready=ready))
                    if args.mode == 'load' and now-last_vt_poll >= 1 and not stop_sent.is_set():
                        with fifo.open('a') as f:
                            f.write('stats cache_audit_poll\n')
                        last_vt_poll = now
                    coverage_ready = geometry_coverage == (0,0,0)
                    if args.bounded_paging:
                        coverage_ready = (geometry_coverage is not None and geometry_coverage[0] == 0 and
                            visible_coverage is not None and visible_coverage[0] > 0 and
                            visible_coverage[1] == 0 and visible_coverage[2] == visible_coverage[3])
                    settled = state == 3 and resident > 0 and inflight == 0 and ready and (args.mode != 'load' or (page_inflight == 0 and coverage_ready and vt_ready and encoded_ready))
                    if settled and resident == previous:
                        stable_since = stable_since or now
                    else:
                        stable_since = None
                    previous = resident
                    if stable_since and now-max(stable_since,last_cache) >= 15 and not stop_sent.is_set():
                        readiness = max(stable_since,last_cache)-start
                        readiness_phases.append(dict(ready_seconds=readiness, confirmed_seconds=now-start,
                            camera_turn_seconds=turn_seconds))
                        if turn_camera is not None and turn_seconds is None:
                            with fifo.open('a') as f:
                                f.write('stats cache_audit_before_turn\ncam ' + ' '.join(map(str,turn_camera)) + '\n')
                            turn_seconds = now-start
                            stable_since = None
                            page_inflight = None
                            geometry_coverage = None
                            visible_coverage = None
                            vt_ready = False
                        else:
                            complete = True
                            streamer_idle_seconds = readiness
                            quit_run()
            code = proc.wait()
    finally:
        deadline.cancel()
    keys = sorted({e['key'] for e in events if e['outcome'] == 'hit' or e['compiled']})
    expected_result = json.loads(args.prepared.read_text()) if args.prepared else None
    expected = expected_result['keys'] if expected_result else None
    prepared_hashes = {e['hash'] for e in prepared_events}
    expected_prepared = {e['hash'] for e in expected_result.get('prepared_events', [])} if expected_result else set()
    result = dict(mode=args.mode, exit_code=code, completed=complete, elapsed_seconds=time.monotonic()-start,
        streamer_idle_seconds=streamer_idle_seconds,
        first_cache_seconds=events[0]["seconds"] if events else None,
        last_cache_seconds=events[-1]["seconds"] if events else None,
        outcomes=dict(collections.Counter(e['outcome'] for e in events)), compilations=sum(e['compiled'] for e in events),
        keys=keys, expected_keys_missing=sorted(set(expected or [])-set(keys)),
        unexpected_keys=sorted(set(keys)-set(expected)) if expected is not None else [],
        identity_sources=dict(identity_sources),
        prepared_surface_reuse=dict(prepared_surface_reuse),
        prepared_hashes_missing=sorted(expected_prepared-prepared_hashes),
        prepared_hashes_unexpected=sorted(prepared_hashes-expected_prepared) if expected_prepared else [],
        geometry_coverage=geometry_coverage, visible_coverage=visible_coverage, bounded_paging=args.bounded_paging,
        vt_samples=vt_samples, encoded_samples=encoded_samples, readiness_phases=readiness_phases, visible_sector_samples=visible_sector_samples, visible_detail_samples=visible_detail_samples,
        events=events, prepared_events=prepared_events, prepared_writes=prepared_writes, statuses=statuses, failures=failures)
    result['valid'] = (not failures and code == 0 and complete and bool(keys) and not result['expected_keys_missing'] and
        not result['unexpected_keys'] and not result['outcomes'].get('failed',0) and
        (args.mode == 'prepare' or (result['compilations']==0 and not result['outcomes'].get('missing',0))))
    if args.identity_cache and not args.identity_cook:
        result['valid'] = bool(result['valid'] and identity_sources['manifest'] > 0 and identity_sources['resolved'] == 0)
    if args.vt_cache:
        final = encoded_samples[-1] if encoded_samples else {}
        result['valid'] = bool(result['valid'] and encoded_ready and final and
            (args.vt_cook or final.get('misses') == 0))
    if args.prepared_sectors:
        result['valid'] = (result['valid'] and bool(prepared_events) and
            not result['prepared_hashes_missing'] and not result['prepared_hashes_unexpected'] and
            (all(e['outcome']=='hit' for e in prepared_events) if args.mode!='prepare' else
             all(e['saved'] for e in prepared_writes) and
             {e['hash'] for e in prepared_events if e['outcome']!='hit'} <= {e['hash'] for e in prepared_writes if e['saved']}))
    (out / 'result.json').write_text(json.dumps(result, indent=2))
    print(json.dumps({k:v for k,v in result.items() if k not in ['events','statuses','keys','prepared_events','prepared_writes','vt_samples','encoded_samples']},indent=2))
    return 0 if result['valid'] else 1

if __name__ == '__main__':
    raise SystemExit(main())
