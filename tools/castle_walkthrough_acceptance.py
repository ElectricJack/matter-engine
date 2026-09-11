#!/usr/bin/env python3
"""Feedback-driven native castle doorway/stair acceptance; standard library only.

Prepare (any OS): python tools/castle_walkthrough_acceptance.py --manifest ... \
    --world CastleGridGallery --output /tmp/castle-route --prepare-only
Run (native Windows Python, prebuilt PhysX editor): py -3 tools/castle_walkthrough_acceptance.py \
    --manifest build/qa/castle-grid/courtyard-detailed-manifest.json \
    --world CastleGridGallery --output C:/tmp/castle-walk-01 --start-waypoint 1

The authored player must spawn at route.json's suggested_spawn. There is no
teleport/spawn command in this driver. One initial sim stop restores the authored
snapshot; subsequent traversal uses only persistent character intent and fixed
physics steps. --offset translates manifest coordinates into the scene.

Current editor compatibility: river-player is the hardcoded character identity.
--player validates identity; it cannot select a different entity until the editor
supports selection. --height/--radius/--speed must match that authored controller.

The compiler's walkRoute may shortcut a whole staircase. This driver expands its
stair.route.waypoints, but DOES NOT repair invalid ground approaches. --route-file
accepts a JSON array of local [x, floorY, z] waypoints (or {"waypoints": [...]});
it must retain the full selected staircase suffix. Use it for explicit reviewed
approach corrections, recorded with provenance. No inferred pathfinding occurs.

Pass requires actual telemetry, an internal doorway crossing, grounded waypoint
arrivals, every stair route waypoint, continuous bounded motion, height progression,
no jumps, and current screenshots. Prepare/self-test never imply native acceptance.
This proves the selected route, not every castle room or every collision surface.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
BAD_LOG = re.compile(r'\btimeout\b|\bFATAL\b|Validation Error|VUID-|character: failed|cmd: unrecognized|'
                     r'dispatch failed|event:.*aborted|\[error\]|validation errors:\s*[1-9]|'
                     r'PhysX[^\n]*(?:disabled|fallback|unavailable)|'
                     r'(?:terrain|collision)[^\n]*(?:fallback|failed)', re.I)
DT = 1 / 60


class EvidenceError(RuntimeError):
    pass


def require(ok, message):
    if not ok:
        raise EvidenceError(message)


def read_json(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def vector(value):
    return (isinstance(value, list) and len(value) == 3 and
            all(type(v) in (int, float) and math.isfinite(v) for v in value))


def distance(a, b):
    return math.hypot(a[0] - b[0], a[2] - b[2])


def near(a, b):
    return max(abs(x - y) for x, y in zip(a, b)) < 1e-5


def authored_hash(name):
    value = 14695981039346656037
    for byte in name.encode('utf-8'):
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return value & ((1 << 63) - 1)


def prepare(args):
    manifest = read_json(args.manifest)
    stairs = [s for s in manifest['stairs'] if not args.stair or args.stair in (s['id'], s.get('sourceId'))]
    require(stairs, 'selected staircase not found')
    stair = stairs[0]
    routes = [r for r in manifest['walkRoute'] if r['roomId'] == stair['upperRoomId']]
    require(routes, 'no walkRoute to selected upper stair room')
    route = routes[0]
    stair_points = stair['route']['waypoints']
    points = route['waypoints']
    start = next((i for i, p in enumerate(points) if near(p, stair_points[0])), None)
    require(start is not None, 'walkRoute does not visit selected lower landing')
    points = points[:start] + stair_points
    if args.route_file:
        override = read_json(args.route_file)
        points = override.get('waypoints') if isinstance(override, dict) else override
        require(isinstance(points, list) and len(points) >= len(stair_points), 'invalid override route')
        require(all(vector(p) for p in points), 'override contains nonfinite or malformed waypoint')
        require(all(near(a, b) for a, b in zip(points[-len(stair_points):], stair_points)),
                'override must retain entire selected stair.route.waypoints suffix')
    require(all(vector(p) for p in points), 'manifest contains invalid route coordinates')
    require(0 <= args.start_waypoint < len(points) - len(stair_points),
            'start waypoint must precede selected staircase')
    points = points[args.start_waypoint:]
    portals = []
    for traversal in route['traversals']:
        if 'outside' in (traversal['fromRoomId'], traversal['toRoomId']):
            continue
        for portal in manifest['portals']:
            thresholds = portal.get('roomThresholds', {})
            a = thresholds.get(traversal['fromRoomId'])
            b = thresholds.get(traversal['toRoomId'])
            if (portal['kind'] in ('door', 'arch') and a and b and
                    near(a, traversal['from']) and near(b, traversal['to'])):
                portals.append(portal)
    require(portals, 'route contains no authored internal door/arch to prove')
    offset = args.offset
    translate = lambda p: [p[i] + offset[i] for i in range(3)]
    require(stair_points[-1][1] > stair_points[0][1], 'selected staircase must ascend')
    return {
        'manifest': str(Path(args.manifest).resolve()), 'manifest_sha256': sha(args.manifest),
        'world': args.world, 'player': args.player, 'stair_id': stair['id'],
        'offset': offset, 'waypoints': list(map(translate, points)),
        'stair_waypoints': list(map(translate, stair_points)), 'portals': portals,
        'height': args.height, 'radius': args.radius, 'speed': args.speed,
        'suggested_spawn': translate([points[0][0], points[0][1] + args.height / 2 + .05, points[0][2]]),
        'route_override': str(Path(args.route_file).resolve()) if args.route_file else None,
        'route_override_sha256': sha(args.route_file) if args.route_file else None,
        'note': 'Planned route only. Ground approach may intersect stair flights; native traversal is the gate.'}


def validate_status(row, args):
    for field in ('position', 'velocity', 'direction'):
        require(vector(row.get(field)), f'invalid {field} telemetry')
    for field in ('scene_id', 'generation', 'fixed_ticks', 'jumps_consumed', 'jumps_started'):
        require(type(row.get(field)) is int and 0 <= row[field] < 2**64, f'invalid {field}')
    for field in ('grounded', 'walk_enabled', 'jump_pending', 'sprint'):
        require(type(row.get(field)) is bool, f'invalid {field}')
    require(row.get('authored_id') == args.player and row['scene_id'] == authored_hash(args.player),
            'wrong authored character identity')
    require(row['generation'] > 0, 'invalid character generation')
    require(row['jumps_consumed'] == row['jumps_started'] == 0 and not row['jump_pending'],
            'unexpected jump during walking acceptance')


def validate_motion(before, after, ticks, args, settling=False):
    validate_status(after, args)
    require(after['generation'] == before['generation'], 'character generation changed')
    require(after['fixed_ticks'] - before['fixed_ticks'] == ticks, 'physics tick count mismatch')
    require(after['mode'] == 'pause' and after['walk_enabled'] and not after['sprint'],
            'character lost paused walking ownership')
    require(distance(before['position'], after['position']) <= args.speed * DT * ticks + .035,
            'horizontal movement exceeded normal controller speed (teleport or configuration mismatch)')
    if not settling:
        require(abs(after['position'][1] - before['position'][1]) <= .4 + args.speed * DT * ticks * 1.2,
                'discontinuous vertical movement')


def crossing(portal, before, after, args):
    """A measured segment crosses the finite opening plane inside capsule clearance."""
    a, b = portal['thresholds']
    center = [(a[i] + b[i]) / 2 + args.offset[i] for i in range(3)]
    length = distance(a, b)
    if length < 1e-6:
        return False
    nx, nz = (b[0] - a[0]) / length, (b[2] - a[2]) / length
    signed = lambda p: (p[0] - center[0]) * nx + (p[2] - center[2]) * nz
    sa, sb = signed(before['position']), signed(after['position'])
    if sa * sb > 0 or abs(sa - sb) < 1e-7:
        return False
    t = sa / (sa - sb)
    p = [before['position'][i] + t * (after['position'][i] - before['position'][i]) for i in range(3)]
    lateral = abs((p[0] - center[0]) * -nz + (p[2] - center[2]) * nx)
    foot = p[1] - args.height / 2
    return (lateral + args.radius <= portal['clearWidth'] / 2 + .025 and
            abs(foot - center[1]) <= .2 and args.height <= portal['clearHeight'] and
            before['grounded'] and after['grounded'])


class NativeSession:
    def __init__(self, args):
        self.args = args
        self.output = Path(args.output).resolve()
        self.fifo = self.output / 'commands.txt'
        self.fifo.write_text('', encoding='utf-8')
        self.rows = {}
        self.lines = []
        self.error = None
        self.condition = threading.Condition()
        self.proc = None
        self.serial = 0
        self.deadline = time.monotonic() + args.timeout
        self.started_ns = time.time_ns()

    def send(self, *commands):
        with self.fifo.open('a', encoding='utf-8', newline='\n') as stream:
            stream.write('\n'.join(commands) + '\n')
            stream.flush()

    def launch(self):
        env = {k: v for k, v in os.environ.items() if not k.upper().startswith('MATTER_')}
        env.update(MATTER_WORLD=self.args.world, MATTER_CMD_FIFO=str(self.fifo),
                   MATTER_HIDE_UI='1', MATTER_VK_VALIDATION='1',
                   MATTER_WINDOW_WIDTH='1280', MATTER_WINDOW_HEIGHT='720',
                   TMP=tempfile.gettempdir(), TEMP=tempfile.gettempdir())
        self.proc = subprocess.Popen([str(Path(self.args.editor).resolve())], cwd=ROOT / 'MatterEditor',
                                     env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     text=True, encoding='utf-8', errors='replace', bufsize=1)
        self.reader = threading.Thread(target=self.read_output, daemon=True)
        self.reader.start()

    def read_output(self):
        with (self.output / 'log.txt').open('w', encoding='utf-8') as log:
            for line in self.proc.stdout:
                log.write(line)
                log.flush()
                with self.condition:
                    self.lines.append(line)
                    if BAD_LOG.search(line):
                        self.error = line.strip()
                    if line.startswith('character_status '):
                        try:
                            row = json.loads(line[len('character_status '):])
                            label = row['label']
                            require(label not in self.rows, 'duplicate telemetry label')
                            self.rows[label] = row
                            with (self.output / 'telemetry.jsonl').open('a', encoding='utf-8') as out:
                                out.write(json.dumps(row) + '\n')
                        except (ValueError, KeyError, EvidenceError) as error:
                            self.error = str(error)
                    self.condition.notify_all()

    def status(self, commands=()):
        self.serial += 1
        label = f'sample_{self.serial:06d}'
        self.send(*commands, f'character status {label}')
        with self.condition:
            while label not in self.rows:
                require(self.error is None, f'native error: {self.error}')
                require(self.proc.poll() is None, 'editor exited before telemetry')
                require(time.monotonic() < self.deadline, 'native run timeout waiting for ' + label)
                self.condition.wait(min(.25, max(.01, self.deadline - time.monotonic())))
            require(self.error is None, f'native error: {self.error}')
            row = self.rows[label]
        validate_status(row, self.args)
        return row

    def step(self, previous, dx=0, dz=0, ticks=4, settling=False):
        commands = [f'character intent {dx:.9f} {dz:.9f} 0']
        for _ in range(ticks):
            commands.extend(('step', 'wait_frames 1'))
        row = self.status(commands)
        validate_motion(previous, row, ticks, self.args, settling)
        return row

    def shot(self, name):
        path = (self.output / f'{name}.png').as_posix()
        self.status([f'shot_now {path}', 'wait_frames 2'])
        return path

    def close(self):
        if self.proc and self.proc.poll() is None:
            self.send('character intent 0 0 0', 'pause', 'quit')
            try:
                self.proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                subprocess.run(['taskkill', '/F', '/T', '/PID', str(self.proc.pid)],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
                self.proc.wait(timeout=10)
        if self.proc:
            self.reader.join(timeout=5)


def verify_shot(path, session):
    path = Path(path)
    data = path.read_bytes()
    require(data[:8] == b'\x89PNG\r\n\x1a\n' and data[12:16] == b'IHDR', 'invalid screenshot PNG')
    require(all(v > 0 for v in struct.unpack('>II', data[16:24])), 'empty screenshot')
    require(path.with_suffix(path.suffix + '.done').read_text().strip() == 'captured', 'missing capture completion')
    require(path.stat().st_mtime_ns >= session.started_ns, 'stale screenshot')
    require(any('screenshot written to ' + path.as_posix() in line.replace('\\', '/')
                for line in session.lines), 'missing screenshot acknowledgement')


def execute(args, plan):
    require(os.name == 'nt', 'native run requires Windows Python; --prepare-only works on any OS')
    require(args.player == 'river-player', 'current editor only binds river-player')
    require(not re.search(r'\s', str(Path(args.output).resolve())), 'screenshot output path must contain no whitespace')
    require(Path(args.editor).is_file(), 'editor executable missing; build it separately')
    cache = Path(args.cmake_cache)
    require(re.search(r'^MATTER_ENABLE_PHYSX:BOOL=ON$', cache.read_text(), re.M),
            'matching native CMake cache must enable PhysX')
    session = NativeSession(args)
    result = {'status': 'failed', 'native_executed': False, 'started_ns': session.started_ns,
              'editor_sha256': sha(args.editor), 'cmake_cache_sha256': sha(cache),
              'manifest_sha256': plan['manifest_sha256'], 'world': args.world, 'player': args.player,
              'door_crossings': [], 'waypoint_arrivals': [], 'screenshots': []}
    try:
        session.send(f'wait_event bake.finished {args.timeout}', 'wait_idle 120', 'sim stop', 'wait_frames 2')
        session.launch()
        result['native_executed'] = True
        spawn = session.status()
        log = ''.join(session.lines)
        for marker in ('viewer: bake ready', 'event: bake.finished', 'idle: settled after'):
            require(marker in log, 'missing native readiness marker: ' + marker)
        require(distance(spawn['position'], plan['waypoints'][0]) <= .2, 'authored spawn is not at route start; see suggested_spawn')
        row = session.status(['character walk on', 'pause', 'character intent 0 0 0', 'wait_frames 2'])
        require(row['generation'] == spawn['generation'], 'generation changed while enabling walking')
        row = session.step(row, ticks=90, settling=True)
        require(row['grounded'], 'authored player did not settle onto real collision')
        base = row
        require(abs(row['position'][1] - args.height / 2 - plan['waypoints'][0][1]) <= .2,
                'spawn settled at wrong floor elevation')
        result['screenshots'].append(session.shot('start'))
        samples = [row]
        grounded_count = 0
        sample_count = 0
        for index, target in enumerate(plan['waypoints'][1:], 1):
            origin = plan['waypoints'][index - 1]
            best_distance = distance(row['position'], target)
            stalled = 0
            while distance(row['position'], target) > .075:
                before = row
                gap = distance(before['position'], target)
                fraction = min(1, gap / (args.speed * DT * 4))
                dx = (target[0] - before['position'][0]) / gap * fraction
                dz = (target[2] - before['position'][2]) / gap * fraction
                row = session.step(before, dx, dz)
                samples.append(row)
                sample_count += 1
                grounded_count += int(row['grounded'])
                length2 = distance(origin, target) ** 2
                t = (((row['position'][0] - origin[0]) * (target[0] - origin[0]) +
                      (row['position'][2] - origin[2]) * (target[2] - origin[2])) / length2) if length2 else 1
                t = max(0, min(1, t))
                expected_y = origin[1] + (target[1] - origin[1]) * t
                # Discrete treads plus capsule radius can raise feet ahead of centerline.
                tolerance = .25 + args.radius * abs(target[1] - origin[1]) / max(.01, distance(origin, target))
                require(abs(row['position'][1] - args.height / 2 - expected_y) <= tolerance,
                        f'waypoint {index}: feet left planned floor/stair elevation')
                remaining = distance(row['position'], target)
                stalled = 0 if remaining < best_distance - .025 else stalled + 1
                best_distance = min(best_distance, remaining)
                require(stalled < 90, f'waypoint {index}: stalled against collision')
                for portal in plan['portals']:
                    if portal['id'] not in [d['portal_id'] for d in result['door_crossings']] and crossing(portal, before, row, args):
                        result['door_crossings'].append({'portal_id': portal['id'], 'before': before['label'], 'after': row['label']})
                        result['screenshots'].append(session.shot(f'door-{len(result["door_crossings"]):02d}'))
            row = session.step(row, ticks=4)
            require(row['grounded'], f'waypoint {index}: not grounded at arrival')
            require(abs(row['position'][1] - args.height / 2 - target[1]) <= .3,
                    f'waypoint {index}: wrong arrival elevation')
            result['waypoint_arrivals'].append({'index': index, 'target': target, 'telemetry': row['label']})
            if any(near(target, p) for p in plan['stair_waypoints']):
                result['screenshots'].append(session.shot(f'stair-{index:02d}'))
        require(result['door_crossings'], 'no measured internal doorway crossing')
        require(sample_count > 0 and grounded_count / sample_count >= .75, 'insufficient grounded walking samples')
        rise = plan['stair_waypoints'][-1][1] - plan['stair_waypoints'][0][1]
        require(abs(row['position'][1] - base['position'][1] - rise) <= .3, 'wrong net ground-to-upper rise')
        for fraction in (.25, .5, .75):
            require(any(abs(s['position'][1] - args.height / 2 -
                            (plan['stair_waypoints'][0][1] + rise * fraction)) <= .35 for s in samples),
                    'missing intermediate stair height progression')
        final = session.step(row, ticks=20)
        require(final['grounded'] and distance(final['position'], row['position']) < .05 and
                abs(final['position'][1] - row['position'][1]) < .05 and
                abs(final['position'][1] - args.height / 2 - plan['waypoints'][-1][1]) <= .3,
                'unstable upper landing')
        result.update(status='passed', final_telemetry=final, grounded_sample_fraction=grounded_count / sample_count,
                      tested_fixed_ticks=final['fixed_ticks'] - base['fixed_ticks'])
    except Exception as error:
        result['error'] = str(error)
    finally:
        try:
            session.close()
        except Exception as error:
            result.update(status='failed', cleanup_error=str(error))
        result['finished_ns'] = time.time_ns()
        result['editor_exit_code'] = session.proc.returncode if session.proc else None
        result['commands_sha256'] = sha(session.fifo)
        if result['status'] == 'passed':
            try:
                require(result['editor_exit_code'] == 0, 'editor did not exit cleanly')
                require(session.error is None, f'native log error: {session.error}')
                for path in result['screenshots']:
                    verify_shot(path, session)
            except Exception as error:
                result.update(status='failed', error=str(error))
        write_json(Path(args.output) / 'result.json', result)
    return result


def self_test():
    from types import SimpleNamespace
    args = SimpleNamespace(player='river-player', speed=4.5, radius=.4, height=1.8, offset=[0, 0, 0])
    def row(x, ticks):
        return dict(label='test', authored_id=args.player, scene_id=authored_hash(args.player), generation=1,
                    fixed_ticks=ticks, position=[x, .9, 0], velocity=[0, 0, 0], direction=[1, 0, 0],
                    jumps_consumed=0, jumps_started=0, grounded=True, walk_enabled=True,
                    jump_pending=False, sprint=False, mode='pause')
    require(authored_hash('river-player') == 1317415599531854025, 'identity hash mismatch')
    a, b = row(-.15, 0), row(.15, 4)
    validate_motion(a, b, 4, args)
    portal = dict(thresholds=[[-.3, 0, 0], [.3, 0, 0]], clearWidth=1.3, clearHeight=2.4)
    require(crossing(portal, a, b, args), 'real doorway crossing rejected')
    for bad in (row(10, 4), row(.15, 5), dict(b, generation=2), dict(b, jumps_started=1),
                dict(b, position=[.15, 8, 0]), dict(b, position=[float('nan'), .9, 0]),
                dict(b, scene_id=0), dict(b, mode='play')):
        try:
            validate_motion(a, bad, 4, args)
        except EvidenceError:
            pass
        else:
            raise EvidenceError('invalid motion accepted')
    require(not crossing(portal, dict(a, position=[-.15, .9, 2]), dict(b, position=[.15, .9, 2]), args),
            'wall jamb incorrectly counted as doorway')
    require(not crossing(portal, dict(a, grounded=False), b, args), 'airborne doorway accepted')
    print('Self-tests passed (synthetic validator checks only; native acceptance NOT RUN).')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--manifest', type=Path)
    parser.add_argument('--world')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--editor', type=Path, default=ROOT / 'MatterEditor/build/windows-msvc/editor.exe')
    parser.add_argument('--cmake-cache', type=Path, default=ROOT / 'MatterEditor/build/cmake/windows-msvc/relwithdebinfo/CMakeCache.txt')
    parser.add_argument('--player', default='river-player')
    parser.add_argument('--stair')
    parser.add_argument('--route-file', type=Path)
    parser.add_argument('--offset', nargs=3, type=float, default=[0, 0, 0])
    parser.add_argument('--height', type=float, default=1.8)
    parser.add_argument('--radius', type=float, default=.4)
    parser.add_argument('--speed', type=float, default=4.5)
    parser.add_argument('--timeout', type=int, default=1800)
    parser.add_argument('--start-waypoint', type=int, default=0)
    parser.add_argument('--prepare-only', action='store_true')
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    require(args.manifest and args.world and args.output, '--manifest, --world and --output are required')
    require(all(math.isfinite(v) and v > 0 for v in (args.height, args.radius, args.speed, args.timeout)), 'invalid controller dimensions/speed/timeout')
    require(all(math.isfinite(v) for v in args.offset), 'nonfinite offset')
    require(not args.output.exists() or not any(args.output.iterdir()), 'output directory must be new or empty')
    plan = prepare(args)
    args.output.mkdir(parents=True, exist_ok=True)
    write_json(args.output / 'route.json', plan)
    if args.prepare_only:
        result = {'status': 'prepared', 'native_executed': False, 'suggested_spawn': plan['suggested_spawn']}
        write_json(args.output / 'result.json', result)
    else:
        result = execute(args, plan)
    print(json.dumps(result, indent=2))
    return 0 if result['status'] in ('prepared', 'passed') else 1


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (EvidenceError, OSError, ValueError, KeyError) as error:
        print(f'castle walkthrough: {error}', file=sys.stderr)
        sys.exit(1)
