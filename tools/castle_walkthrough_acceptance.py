#!/usr/bin/env python3
"""Feedback-driven native castle doorway/stair acceptance; standard library only.

Prepare (any OS): python tools/castle_walkthrough_acceptance.py --manifest ... \
    --world CastleGridGallery --output /tmp/castle-route --prepare-only
Run (native Windows Python, prebuilt PhysX editor): py -3 tools/castle_walkthrough_acceptance.py \
    --manifest path/to/site-manifest.json \
    --world CastleClusteredCourt --output C:/tmp/castle-walk-01 --run

Native execution shares castle_scene_capture.PublicationGate: numeric completed
publication, a clean bake, viewer readiness, then a fresh wait_idle acknowledgment
are required before ANY simulation/player/camera controls. Cached publication
placeholders cannot release walking. publication.json and result.json retain
the receipt; timeout/error never starts traversal. Only closed-world scenes
are supported by this publication gate.

The authored player must spawn at route.json's suggested_spawn. There is no
teleport/spawn command in this driver. One initial sim stop restores the authored
snapshot; subsequent traversal uses only persistent character intent and fixed
physics steps. --offset translates manifest coordinates into the scene.

Current editor compatibility: river-player is the hardcoded character identity.
--player validates identity; it cannot select a different entity until the editor
supports selection. --height/--radius/--speed must match that authored controller.

Supports matter.castle-manifest/v1 and matter.castle-site-manifest/v1. Site stairs
use wing-qualified IDs, e.g. --stair keep:stair-1; routes start at site.spawn and
retain every authored ascent. Wing yaw transforms match engine +Y rotation.
Legacy local walkRoute stair shortcuts are expanded from stair.route.waypoints.
Neither path repairs invalid approaches. --route-file accepts manifest-space
[x, floorY, z] waypoints (or {"waypoints": [...]}); it must retain the full selected
stair sequence (a suffix for legacy local plans). Overrides retain provenance.
Site overrides still begin at the authored spawn. No inferred pathfinding occurs.

Pass requires actual telemetry, every planned internal doorway/connector mouth
crossing (the main outside entry does not count), grounded waypoint
arrivals, every stair route waypoint, continuous bounded motion, height progression,
no jumps, and current screenshots. Prepare/self-test never imply native acceptance.
This proves the selected route, not every castle room or every collision surface.
"""
import argparse
import hashlib
import importlib.util
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
# Load the sibling helper by path so both CLI execution and importlib-based
# prepare-only clients share its publication contract from any working directory.
_CAPTURE_SPEC = importlib.util.spec_from_file_location(
    'castle_walk_publication', Path(__file__).with_name('castle_scene_capture.py'))
_CAPTURE = importlib.util.module_from_spec(_CAPTURE_SPEC)
_CAPTURE_SPEC.loader.exec_module(_CAPTURE)
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


def positive_int(value):
    number = int(value)
    if number <= 0:
        raise argparse.ArgumentTypeError('must be a positive integer')
    return number


def static_buffer_reserve_env(args):
    result = {}
    for option, variable in (
            ('static_vertex_reserve_mb', 'MATTER_VK_STATIC_RESERVE_VERTEX_MB'),
            ('static_index_reserve_mb', 'MATTER_VK_STATIC_RESERVE_INDEX_MB')):
        value = getattr(args, option, None)
        if value is not None:
            require(type(value) is int and value > 0, option + ' must be a positive integer')
            result[variable] = str(value)
    return result


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


def transform_point(frame, point):
    """Engine +Y yaw: +X rotates toward -Z; never snap world coordinates."""
    require(vector(frame.get('origin')) and vector(point), 'invalid wing frame/point')
    yaw = frame.get('yawDeg')
    require(type(yaw) in (int, float) and math.isfinite(yaw), 'invalid wing yaw')
    c, s = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
    x, y, z = point
    origin = frame['origin']
    return [origin[0] + c * x + s * z, origin[1] + y, origin[2] - s * x + c * z]


def route_has_sequence(points, sequence):
    return any(all(near(a, b) for a, b in zip(points[i:i + len(sequence)], sequence))
               for i in range(len(points) - len(sequence) + 1))


def normalize_manifest(manifest):
    """Read-only acceptance geometry, in manifest world space (before --offset)."""
    schema = manifest.get('schema')
    require(schema in ('matter.castle-manifest/v1', 'matter.castle-site-manifest/v1'),
            f'unsupported castle manifest schema: {schema}')
    if schema == 'matter.castle-manifest/v1':
        portals = [dict(p, proof_kind='internal') for p in manifest['portals']
                   if p['kind'] in ('door', 'arch') and 'outside' not in p['rooms']]
        return dict(site=False, stairs=manifest['stairs'], routes=manifest['walkRoute'], portals=portals)
    require(vector(manifest.get('spawn')), 'site manifest requires finite authored spawn')
    stairs, portals = [], []
    for wing in manifest['wings']:
        frame, local = wing['frame'], wing['manifest']
        require(local.get('schema') == 'matter.castle-manifest/v1', 'invalid wing manifest schema')
        qualify = lambda value: 'outside' if value == 'outside' else f"{wing['id']}:{value}"
        transform = lambda point: transform_point(frame, point)
        for stair in local['stairs']:
            stairs.append(dict(
                id=qualify(stair['id']), sourceId=qualify(stair.get('sourceId', stair['id'])),
                wing_id=wing['id'], width=stair['width'],
                lowerRoomId=qualify(stair['lowerRoomId']), upperRoomId=qualify(stair['upperRoomId']),
                lowerLevelId=qualify(stair['lowerLevelId']), upperLevelId=qualify(stair['upperLevelId']),
                route={'waypoints': list(map(transform, stair['route']['waypoints']))},
                flights=[dict(id=qualify(f['id']), width=f.get('width', stair['width']),
                              centerline=list(map(transform, f['centerline']))) for f in stair['flights']]))
        for portal in local['portals']:
            if portal['kind'] not in ('door', 'arch') or 'outside' in portal['rooms']:
                continue
            portals.append(dict(
                id=qualify(portal['id']), sourceId=qualify(portal['sourceId']),
                levelId=qualify(portal['levelId']), rooms=list(map(qualify, portal['rooms'])),
                kind=portal['kind'], proof_kind='internal',
                thresholds=list(map(transform, portal['thresholds'])),
                roomThresholds={qualify(room): transform(point)
                                for room, point in portal.get('roomThresholds', {}).items()},
                clearWidth=portal['clearWidth'], clearHeight=portal['clearHeight']))
    # A connector edge spans two distinct wall planes. Its graph thresholds alone
    # would test an imaginary plane in the connector's middle, missing both mouths.
    for connector in manifest['connectors']:
        require(len(connector['mouths']) == 2, 'connector must have two mouths')
        for side, mouth in zip(('a', 'b'), connector['mouths']):
            entry = manifest['entry']
            require((mouth['wing'], mouth['level'], mouth['portalId']) !=
                    (entry['wing'], entry['level'], entry['portalId']),
                    'site entry cannot also be a consumed connector mouth')
            a, b = mouth['segment']
            width = math.hypot(b[0] - a[0], b[1] - a[1])
            portals.append(dict(
                id=f"connector:{connector['id']}:mouth:{side}", sourceId=f"{mouth['wing']}:{mouth['portalId']}",
                connector_id=connector['id'], wing_id=mouth['wing'], proof_kind='connector-mouth',
                thresholds=[mouth['inside'], mouth['outside']],
                clearWidth=width, clearHeight=mouth['clearHeight']))
    require(len({s['id'] for s in stairs}) == len(stairs), 'duplicate qualified staircase IDs')
    require(len({p['id'] for p in portals}) == len(portals), 'duplicate proof portal IDs')
    return dict(site=True, stairs=stairs, routes=manifest['walkRoutes'], portals=portals,
                spawn=manifest['spawn'])


def opening_intersection(portal, a, b, height, radius, offset):
    """Intersect floor points with the finite clear opening; return point or None."""
    thresholds = portal['thresholds']
    require(len(thresholds) == 2 and all(vector(p) for p in thresholds), 'invalid portal thresholds')
    u, v = thresholds
    length = distance(u, v)
    require(length > 1e-6, 'portal thresholds do not define a finite wall normal')
    center = [(u[i] + v[i]) / 2 + offset[i] for i in range(3)]
    nx, nz = (v[0] - u[0]) / length, (v[2] - u[2]) / length
    signed = lambda p: (p[0] - center[0]) * nx + (p[2] - center[2]) * nz
    sa, sb = signed(a), signed(b)
    if sa * sb > 1e-12 or abs(sa - sb) < 1e-7:
        return None
    t = sa / (sa - sb)
    if not -1e-7 <= t <= 1 + 1e-7:
        return None
    p = [a[i] + t * (b[i] - a[i]) for i in range(3)]
    lateral = abs((p[0] - center[0]) * -nz + (p[2] - center[2]) * nx)
    if (lateral + radius <= portal['clearWidth'] / 2 + .025 and
            abs(p[1] - center[1]) <= .2 and height <= portal['clearHeight']):
        return p
    return None


def planned_crossing(portal, points, args):
    """A wall-plane touch/turnback is not a crossing, even at a route waypoint."""
    u, v = portal['thresholds']
    center = [(u[i] + v[i]) / 2 for i in range(3)]
    signed = lambda p: ((p[0] - center[0]) * (v[0] - u[0]) +
                        (p[2] - center[2]) * (v[2] - u[2]))
    signs = [signed(p) for p in points]
    for i, (a, b) in enumerate(zip(points, points[1:])):
        if opening_intersection(portal, a, b, args.height, args.radius, [0, 0, 0]) is None:
            continue
        left = next((s for s in reversed(signs[:i + 1]) if abs(s) > 1e-7), 0)
        right = next((s for s in signs[i + 1:] if abs(s) > 1e-7), 0)
        if left * right < 0:
            return True
    return False


def prepare(args):
    manifest = normalize_manifest(read_json(args.manifest))
    stairs = [s for s in manifest['stairs'] if not args.stair or args.stair in (s['id'], s.get('sourceId'))]
    require(stairs, 'selected staircase not found (site IDs must be wing-qualified)')
    require(not args.stair or len(stairs) == 1, 'selected staircase ID is ambiguous')
    stair = stairs[0]
    routes = [r for r in manifest['routes'] if r['roomId'] == stair['upperRoomId']]
    require(len(routes) == 1, 'expected exactly one walkRoute to selected upper stair room')
    route = routes[0]
    stair_points = stair['route']['waypoints']
    points = route['waypoints']
    require(stair_points and points and all(vector(p) for p in points + stair_points),
            'manifest contains invalid route coordinates')
    if not manifest['site']:
        # Compatibility for older local manifests with shortcut stair graph edges.
        start = next((i for i, p in enumerate(points) if near(p, stair_points[0])), None)
        require(start is not None, 'walkRoute does not visit selected lower landing')
        points = points[:start] + stair_points
    if args.route_file:
        override = read_json(args.route_file)
        points = override.get('waypoints') if isinstance(override, dict) else override
        require(isinstance(points, list) and len(points) >= len(stair_points), 'invalid override route')
        require(all(vector(p) for p in points), 'override contains nonfinite or malformed waypoint')
        if not manifest['site']:
            require(all(near(a, b) for a, b in zip(points[-len(stair_points):], stair_points)),
                    'override must retain entire selected stair.route.waypoints suffix')
    if manifest['site']:
        require(args.start_waypoint == 0, 'site routes must begin at authored site.spawn; cannot skip waypoints')
        if not near(points[0], manifest['spawn']):
            points = [manifest['spawn']] + points
    else:
        require(0 <= args.start_waypoint < len(points) - len(stair_points),
                'start waypoint must precede selected staircase')
        points = points[args.start_waypoint:]
    points = [p for i, p in enumerate(points) if not i or not near(p, points[i - 1])]
    require(route_has_sequence(points, stair_points), 'route must retain entire selected stair.route.waypoints')
    portals = [p for p in manifest['portals'] if planned_crossing(p, points, args)]
    require(portals, 'route contains no finite internal doorway or connector mouth crossing to prove')
    offset = args.offset
    translate = lambda p: [p[i] + offset[i] for i in range(3)]
    require(stair_points[-1][1] > stair_points[0][1], 'selected staircase must ascend')
    visited = [s for s in manifest['stairs'] if
               route_has_sequence(points, s['route']['waypoints']) or
               route_has_sequence(points, list(reversed(s['route']['waypoints'])))]
    attachments = []
    for visited_stair in visited:
        for flight in visited_stair['flights']:
            a, b = flight['centerline'][0], flight['centerline'][-1]
            length = distance(a, b)
            width = flight.get('width', visited_stair['width'])
            require(length > 0 and math.isfinite(width) and width > 0, 'invalid flight attachment geometry')
            across = [-(b[2] - a[2]) / length, (b[0] - a[0]) / length]
            for center in (a, b):
                attachments.append({'center': translate(center), 'across': across, 'half_width': width / 2,
                                    'stair_id': visited_stair['id'], 'flight_id': flight['id']})
    return {
        'manifest': str(Path(args.manifest).resolve()), 'manifest_sha256': sha(args.manifest),
        'manifest_kind': 'site' if manifest['site'] else 'local',
        'world': args.world, 'player': args.player, 'stair_id': stair['id'],
        'visited_stair_ids': [s['id'] for s in visited], 'target_room_id': stair['upperRoomId'],
        'offset': offset, 'waypoints': list(map(translate, points)),
        'route_net_rise': points[-1][1] - points[0][1],
        'stair_waypoints': list(map(translate, stair_points)), 'stair_attachments': attachments, 'portals': portals,
        'height': args.height, 'radius': args.radius, 'speed': args.speed,
        'suggested_spawn': translate([points[0][0], points[0][1] + args.height / 2 + .05, points[0][2]]),
        'route_override': str(Path(args.route_file).resolve()) if args.route_file else None,
        'route_override_sha256': sha(args.route_file) if args.route_file else None,
        'note': 'Planned route only. Native collision traversal is the acceptance gate.'}


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


def attachment_center(attachment):
    return attachment['center'] if isinstance(attachment, dict) else attachment


def attachment_distance(position, attachment):
    center = attachment_center(attachment)
    if not isinstance(attachment, dict):
        return distance(position, center)
    across = attachment['across']
    lateral = ((position[0] - center[0]) * across[0] +
               (position[2] - center[2]) * across[1])
    lateral = max(-attachment['half_width'], min(attachment['half_width'], lateral))
    closest = [center[0] + lateral * across[0], center[1], center[2] + lateral * across[1]]
    return distance(position, closest)


def validate_flat_support(row, stair_points, args, airborne_ticks):
    if row['grounded']:
        return 0
    require(airborne_ticks <= 30, 'level walking remained airborne beyond a stair attachment transition')
    foot_y = row['position'][1] - args.height / 2
    require(any(attachment_distance(row['position'], p) <= args.radius + .25 and
                abs(foot_y - attachment_center(p)[1]) <= .35 for p in stair_points),
            'level walking lost ground contact away from a stair attachment')
    return airborne_ticks


def crossing(portal, before, after, args):
    """A measured segment crosses the finite opening plane inside capsule clearance."""
    a = [before['position'][0], before['position'][1] - args.height / 2, before['position'][2]]
    b = [after['position'][0], after['position'][1] - args.height / 2, after['position'][2]]
    return (before['grounded'] and after['grounded'] and
            opening_intersection(portal, a, b, args.height, args.radius, args.offset) is not None)



def validate_height_progression(samples, base, final, plan, args):
    rise = plan['stair_waypoints'][-1][1] - plan['stair_waypoints'][0][1]
    require(abs(final['position'][1] - base['position'][1] - plan['route_net_rise']) <= .3,
            'wrong net route start-to-end rise')
    for fraction in (.25, .5, .75):
        require(any(abs(sample['position'][1] - args.height / 2 -
                        (plan['stair_waypoints'][0][1] + rise * fraction)) <= .35 for sample in samples),
                'missing intermediate stair height progression')


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
        self.publication_gate = _CAPTURE.PublicationGate()
        self.publication_released = False
        self.reader_done = False

    def send(self, *commands):
        if not self.publication_released:
            require(all(command == 'quit' or command.startswith('wait_idle ') for command in commands),
                    'player/camera commands are blocked until geometry publication and fresh idle acknowledgment')
        with self.fifo.open('a', encoding='utf-8', newline='\n') as stream:
            stream.write('\n'.join(commands) + '\n')
            stream.flush()

    def launch(self):
        env = {k: v for k, v in os.environ.items() if not k.upper().startswith('MATTER_')}
        env.update(static_buffer_reserve_env(self.args))
        env.update(MATTER_WORLD=self.args.world, MATTER_CMD_FIFO=str(self.fifo),
                   MATTER_HIDE_UI='1', MATTER_HIDE_WINDOW='1', MATTER_IMPOSTOR='0', MATTER_VK_VALIDATION='1',
                   MATTER_WINDOW_WIDTH='1280', MATTER_WINDOW_HEIGHT='720',
                   TMP=tempfile.gettempdir(), TEMP=tempfile.gettempdir())
        self.proc = subprocess.Popen([str(Path(self.args.editor).resolve())], cwd=ROOT / 'MatterEditor',
                                     env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     text=True, encoding='utf-8', errors='replace', bufsize=1)
        self.reader = threading.Thread(target=self.read_output, daemon=True)
        self.reader.start()

    def read_output(self):
        try:
            with (self.output / 'log.txt').open('w', encoding='utf-8') as log:
                for line in self.proc.stdout:
                    log.write(line)
                    log.flush()
                    with self.condition:
                        self.lines.append(line)
                        self.publication_gate.feed(line)
                        if BAD_LOG.search(line):
                            self.error = line.strip()
                        if self.publication_gate.error:
                            self.error = self.publication_gate.error
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
                        if self.error:
                            self.publication_gate.error = self.error
                        self.condition.notify_all()
        except Exception as error:
            with self.condition:
                self.error = self.publication_gate.error = 'log reader failed: ' + str(error)
        finally:
            with self.condition:
                self.reader_done = True
                self.condition.notify_all()

    def await_publication(self):
        """Release walking only after completed publication and a fresh idle ACK."""
        require(self.proc is not None, 'editor has not launched')
        require(not self.publication_released, 'publication barrier already released')
        gate = self.publication_gate
        _CAPTURE.await_gate(self.condition, gate, lambda: gate.published, self.deadline,
                            lambda: not self.reader_done, 'complete geometry publication before walking')
        with self.condition:
            # An idle line arriving during bake/resolve is not the acknowledgment
            # of the idle command issued AFTER the numeric publication barrier.
            gate.idle = False
            self.send(f'wait_idle {self.args.settle_seconds} {self.args.timeout}')
        _CAPTURE.await_gate(self.condition, gate, lambda: gate.published and gate.idle,
                            self.deadline, lambda: not self.reader_done, 'post-publication idle before walking')
        with self.condition:
            require(self.error is None and gate.published and gate.idle,
                    'publication invalidated before walking')
            require(not self.reader_done and self.proc.poll() is None,
                    'editor/log reader exited before walking publication release')
            self.publication_released = True
            receipt = gate.receipt()
        write_json(self.output / 'publication.json', receipt)
        return receipt

    def status(self, commands=()):
        self.serial += 1
        label = f'sample_{self.serial:06d}'
        self.send(*commands, f'character status {label}')
        with self.condition:
            while label not in self.rows:
                require(self.error is None, f'native error: {self.error}')
                require(self.proc.poll() is None and not self.reader_done, 'editor/log reader exited before telemetry')
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
            latest = next(reversed(self.rows.values()), {})
            commands = ['character intent 0 0 0'] if self.publication_released and latest.get('walk_enabled') else []
            if self.publication_released and latest.get('mode') == 'play':
                commands.append('pause')
            self.send(*commands, 'quit')
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
              'static_buffer_reserve_env': static_buffer_reserve_env(args),
              'render_path': 'raster', 'impostors_enabled': False,
              'manifest_sha256': plan['manifest_sha256'], 'world': args.world, 'player': args.player,
              'door_crossings': [], 'waypoint_arrivals': [], 'screenshots': []}
    try:
        session.launch()  # FIFO remains empty throughout geometry publication.
        result['native_executed'] = True
        result['publication_before_walk'] = session.await_publication()
        spawn = session.status(['render_path raster', 'sim stop', 'wait_frames 2'])
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
        flat_count = flat_grounded = stair_count = stair_grounded = 0
        for index, target in enumerate(plan['waypoints'][1:], 1):
            origin = plan['waypoints'][index - 1]
            best_distance = distance(row['position'], target)
            stalled = 0
            flat_airborne_ticks = 0
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
                if abs(target[1] - origin[1]) < 1e-6:
                    flat_count += 1
                    flat_grounded += int(row['grounded'])
                    flat_airborne_ticks = validate_flat_support(
                        row, plan['stair_attachments'], args, flat_airborne_ticks + 4)
                else:
                    stair_count += 1
                    stair_grounded += int(row['grounded'])
                length2 = distance(origin, target) ** 2
                t = (((row['position'][0] - origin[0]) * (target[0] - origin[0]) +
                      (row['position'][2] - origin[2]) * (target[2] - origin[2])) / length2) if length2 else 1
                t = max(0, min(1, t))
                expected_y = origin[1] + (target[1] - origin[1]) * t
                # Discrete treads plus capsule radius can raise feet ahead of centerline.
                tolerance = .25 + args.radius * abs(target[1] - origin[1]) / max(.01, distance(origin, target))
                if any(attachment_distance(row['position'], p) <= args.radius + .25 and
                       abs(expected_y - attachment_center(p)[1]) < 1e-6 for p in plan['stair_attachments']):
                    tolerance = max(tolerance, .35)
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
            # Capsule contact may still be resolving on the first/last riser.
            # Only stair attachments get a bounded additional settle window.
            if any(attachment_distance(target, p) <= args.radius + .25 and
                   abs(target[1] - attachment_center(p)[1]) < 1e-6 for p in plan['stair_attachments']):
                for _ in range(6):
                    if row['grounded']:
                        break
                    row = session.step(row, ticks=4)
            require(row['grounded'], f'waypoint {index}: not grounded at arrival')
            require(distance(row['position'], target) <= .15, f'waypoint {index}: drifted from arrival')
            require(abs(row['position'][1] - args.height / 2 - target[1]) <= .3,
                    f'waypoint {index}: wrong arrival elevation')
            result['waypoint_arrivals'].append({'index': index, 'target': target, 'telemetry': row['label']})
            if any(near(target, p) for p in plan['stair_waypoints']):
                result['screenshots'].append(session.shot(f'stair-{index:02d}'))
        expected_portals = {p['id'] for p in plan['portals']}
        measured_portals = {p['portal_id'] for p in result['door_crossings']}
        require(expected_portals <= measured_portals,
                'missing measured doorway/mouth crossings: ' + ', '.join(sorted(expected_portals - measured_portals)))
        # Stair contact resolution can raise a capsule while its sampled
        # grounded flag is false; four-tick samples can miss the intervening
        # contacts. Gate level walking and every landing, and retain the
        # continuous tread-height, speed, no-jump and final-support checks.
        require(flat_count > 0 and flat_grounded / flat_count >= .9,
                'insufficient grounded level-walking samples')
        validate_height_progression(samples, base, row, plan, args)
        final = session.step(row, ticks=20)
        require(final['grounded'] and distance(final['position'], row['position']) < .05 and
                abs(final['position'][1] - row['position'][1]) < .05 and
                abs(final['position'][1] - args.height / 2 - plan['waypoints'][-1][1]) <= .3,
                'unstable upper landing')
        result.update(status='passed', final_telemetry=final, grounded_sample_fraction=grounded_count / sample_count,
                      flat_grounded_sample_fraction=flat_grounded / flat_count,
                      stair_grounded_sample_fraction=stair_grounded / stair_count if stair_count else None,
                      tested_fixed_ticks=final['fixed_ticks'] - base['fixed_ticks'])
    except Exception as error:
        result['error'] = str(error)
    finally:
        try:
            session.close()
        except Exception as error:
            result.update(status='failed', cleanup_error=str(error))
        result['publication'] = session.publication_gate.receipt()
        result['publication_released'] = session.publication_released
        result['last_telemetry'] = next(reversed(session.rows.values()), None)
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
    attachment = dict(b, grounded=False, position=[0, 1.1, .2])
    validate_flat_support(attachment, [[0, 0, 0]], args, 16)
    for point, ticks in (([4, 1.1, 0], 4), ([0, 1.1, .2], 32)):
        try:
            validate_flat_support(dict(attachment, position=point), [[0, 0, 0]], args, ticks)
        except EvidenceError:
            pass
        else:
            raise EvidenceError('unsupported level walking accepted')
    wide_edge = {'center': [.75, 2, 5], 'across': [1, 0], 'half_width': .75}
    validate_flat_support(dict(attachment, position=[.146, 2.977, 5.282]), [wide_edge], args, 24)
    require(attachment_distance([2.5, 2.9, 5], wide_edge) == 1, 'attachment edge must remain finite')
    # Finite planned crossings handle a waypoint exactly on the plane, while
    # rejecting turnbacks, wall-parallel travel, jambs and the wrong storey.
    require(planned_crossing(portal, [[-1, 0, 0], [0, 0, 0], [1, 0, 0]], args),
            'split doorway crossing rejected')
    for points in ([[-1, 0, 0], [0, 0, 0], [-1, 0, 0]],
                   [[0, 0, -1], [0, 0, 1]], [[-1, 0, 2], [1, 0, 2]],
                   [[-1, 4, 0], [1, 4, 0]]):
        require(not planned_crossing(portal, points, args), 'non-crossing planned route accepted')

    def staircase(index):
        x, y = 2 + index * 6, index * 4
        points = [[x, y, 0], [x + 1, y, 0], [x + 5, y + 4, 0], [x + 6, y + 4, 0]]
        return dict(id=f'stair:s{index}', sourceId=f's{index}', width=2,
                    lowerRoomId=f'r{index}', upperRoomId=f'r{index + 1}',
                    lowerLevelId=f'l{index}', upperLevelId=f'l{index + 1}',
                    route={'waypoints': points}, flights=[dict(id=f'flight:{index}', width=2,
                                                             centerline=points[1:3])])

    def door(id, x, rooms):
        return dict(id=f'portal:{id}', sourceId=id, kind='arch', levelId='l0', rooms=rooms,
                    clearWidth=2, clearHeight=2.8, thresholds=[[x - .3, 0, 0], [x + .3, 0, 0]],
                    roomThresholds={rooms[0]: [x - .3, 0, 0], rooms[1]: [x + .3, 0, 0]})

    def must_fail(fn, message):
        try:
            fn()
        except EvidenceError:
            return
        raise EvidenceError(message)

    for angle in (15, 30, 45):
        frame = {'origin': [11, 2, -7], 'yawDeg': angle}
        angle_radians = math.radians(angle)
        expected = [11 + math.cos(angle_radians), 2, -7 - math.sin(angle_radians)]
        require(near(transform_point(frame, [1, 0, 0]), expected), 'engine yaw convention changed')
        transform = lambda p: transform_point(frame, p)
        far_frame = dict(frame, origin=transform([4, 0, 0]))
        far_transform = lambda p: transform_point(far_frame, p)
        local_stairs = [staircase(0), staircase(1)]
        local = dict(schema='matter.castle-manifest/v1', stairs=local_stairs,
                     portals=[door('upper-room-door', 16, ['r2', 'r3'])], walkRoute=[])
        near_local = dict(schema='matter.castle-manifest/v1', stairs=[],
                          portals=[door('internal', 0, ['entrance', 'hall']),
                                   door('entry', -2, ['outside', 'entrance'])], walkRoute=[])
        def mouth(wing, x, transform):
            a, b = transform([x, 0, -1]), transform([x, 0, 1])
            return dict(wing=wing, level='l0', portalId='link-entry',
                        inside=transform([x - .3, 0, 0]), outside=transform([x + .3, 0, 0]),
                        segment=[[a[0], a[2]], [b[0], b[2]]], clearHeight=2.8)
        points = list(map(transform, [[-1, 0, 0], [.6, 0, 0], [1.7, 0, 0], [4.3, 0, 0]]))
        points += list(map(far_transform, local_stairs[0]['route']['waypoints']))
        points += list(map(far_transform, local_stairs[1]['route']['waypoints'][1:]))
        site = dict(schema='matter.castle-site-manifest/v1', spawn=transform([-1.5, 0, 0]),
                    entry=dict(wing='near', level='l0', portalId='entry'),
                    wings=[dict(id='near', frame=frame, manifest=near_local),
                           dict(id='far', frame=far_frame, manifest=local)],
                    connectors=[dict(id='link', mouths=[mouth('near', 2, transform),
                                                       mouth('far', 0, far_transform)])],
                    walkRoutes=[dict(roomId='far:r2', waypoints=points)])
        normalized = normalize_manifest(site)
        require(normalized['stairs'][1]['upperRoomId'] == 'far:r2', 'rooms not namespaced')
        require(len(normalized['portals']) == 4, 'entry incorrectly included or connector mouth omitted')
        # A single planned segment traverses both connector mouth planes.
        require(all(planned_crossing(p, points, args) for p in normalized['portals']
                    if p['proof_kind'] == 'connector-mouth'), 'connector mouths not geometrically selected')
        with tempfile.TemporaryDirectory(prefix='castle-walk-self-test-') as temp:
            path = Path(temp) / 'site.json'
            write_json(path, site)
            options = SimpleNamespace(**vars(args), manifest=path, world='Test', stair='far:s1',
                                      route_file=None, start_waypoint=0)
            options.offset = [-4, 3, 6]
            plan = prepare(options)
            require(plan['stair_id'] == 'far:stair:s1' and plan['route_net_rise'] == 8,
                    'stacked ascent selected wrong stair or net rise')
            require(plan['visited_stair_ids'] == ['far:stair:s0', 'far:stair:s1'],
                    'earlier ascent attachments omitted')
            require(len(plan['stair_attachments']) == 4 and len(plan['portals']) == 3,
                    'wrong finite attachment/portal count')
            require(near(plan['waypoints'][0], [site['spawn'][i] + options.offset[i] for i in range(3)]),
                    'authored site spawn or offset was not preserved')
            attachment = plan['stair_attachments'][0]
            require(near(attachment['center'], [far_transform([3, 0, 0])[i] + options.offset[i]
                                               for i in range(3)]), 'rotated flight attachment wrong')
            require(abs(attachment['across'][0] - math.sin(angle_radians)) < 1e-8 and
                    abs(attachment['across'][1] - math.cos(angle_radians)) < 1e-8,
                    'attachment width not rotated with flight')
            proof = plan['portals'][0]
            u, v = proof['thresholds']
            measured = lambda p: dict(grounded=True, position=[p[0] + options.offset[0],
                p[1] + options.offset[1] + options.height / 2, p[2] + options.offset[2]])
            require(crossing(proof, measured(u), measured(v), options), 'rotated/offset measured crossing failed')
            jamb_a, jamb_b = measured(u), measured(v)
            for sample in (jamb_a, jamb_b):
                sample['position'][0] += 2 * math.sin(angle_radians)
                sample['position'][2] += 2 * math.cos(angle_radians)
            require(not crossing(proof, jamb_a, jamb_b, options), 'rotated wall jamb counted as passage')
            # The earlier staircase's first attachment must also permit its
            # tightly bounded flat-contact transition in a stacked route.
            center = attachment['center']
            validate_flat_support(dict(grounded=False, position=[center[0], center[1] + 1.05, center[2]]),
                                  plan['stair_attachments'], options, 12)
            base_y = plan['waypoints'][0][1] + options.height / 2
            samples = [{'position': [0, base_y + rise, 0]} for rise in (5, 6, 7)]
            base = {'position': [0, base_y, 0]}
            final = {'position': [0, base_y + 8, 0]}
            validate_height_progression(samples, base, final, plan, options)
            must_fail(lambda: validate_height_progression(samples, base, {'position': [0, base_y + 4, 0]},
                                                         plan, options), 'only one of two ascents accepted')
            must_fail(lambda: validate_height_progression(samples[:1], base, final, plan, options),
                      'selected stair quarter-height proof omitted')
            options.stair = 's1'
            must_fail(lambda: prepare(options), 'unqualified site staircase accepted')
            options.stair, options.start_waypoint = 'far:s1', 1
            must_fail(lambda: prepare(options), 'site spawn skipped')
            options.start_waypoint = 0
            site['walkRoutes'][0]['waypoints'] = points[:-2] + points[-1:]
            write_json(path, site)
            must_fail(lambda: prepare(options), 'shortcut staircase accepted as full site route')
            # Preserve the old local shortcut expansion and exterior exclusion.
            local_route = [[-1, 0, 0], [.6, 0, 0], [2, 0, 0], [8, 4, 0]]
            legacy = dict(schema='matter.castle-manifest/v1', stairs=[staircase(0)],
                          portals=near_local['portals'], walkRoute=[dict(roomId='r1', waypoints=local_route)])
            write_json(path, legacy)
            options.stair = 's0'
            legacy_plan = prepare(options)
            require(legacy_plan['manifest_kind'] == 'local' and legacy_plan['route_net_rise'] == 4 and
                    len(legacy_plan['portals']) == 1, 'legacy route expansion regressed')
    must_fail(lambda: normalize_manifest({'schema': 'future/v2'}), 'unknown schema accepted')
    print('Self-tests passed (synthetic validator checks only; native acceptance NOT RUN).')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--manifest', type=Path)
    parser.add_argument('--world')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--editor', type=Path, default=ROOT / 'MatterEditor/build/windows-msvc/editor.exe')
    parser.add_argument('--cmake-cache', type=Path, default=ROOT / 'MatterEditor/build/cmake/windows-msvc/relwithdebinfo/CMakeCache.txt')
    parser.add_argument('--player', default='river-player')
    parser.add_argument('--stair', help='local stair ID/sourceId, or wing-qualified site ID (e.g. keep:stair-1)')
    parser.add_argument('--route-file', type=Path)
    parser.add_argument('--offset', nargs=3, type=float, default=[0, 0, 0])
    parser.add_argument('--height', type=float, default=1.8)
    parser.add_argument('--radius', type=float, default=.4)
    parser.add_argument('--speed', type=float, default=4.5)
    parser.add_argument('--static-vertex-reserve-mb', type=positive_int,
                        help='explicit native static vertex reservation in MiB')
    parser.add_argument('--static-index-reserve-mb', type=positive_int,
                        help='explicit native static index reservation in MiB')
    parser.add_argument('--timeout', type=int, default=1800)
    parser.add_argument('--settle-seconds', type=float, default=5,
                        help='idle stability after completed numeric publication (default: 5 seconds)')
    parser.add_argument('--start-waypoint', type=int, default=0)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--prepare-only', action='store_true')
    mode.add_argument('--run', action='store_true', help='execute native physical walkthrough (also the default)')
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    require(args.manifest and args.world and args.output, '--manifest, --world and --output are required')
    require(all(math.isfinite(v) and v > 0 for v in (args.height, args.radius, args.speed, args.timeout, args.settle_seconds)), 'invalid controller dimensions/speed/timeout')
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
