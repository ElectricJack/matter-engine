"""Validate deterministic character telemetry and current native captures."""
import argparse
import json
import math
from pathlib import Path
import re
import struct
import sys

LABELS = ('edit', 'grounded', 'walk', 'sprint', 'jump_base', 'latched',
          'paused', 'jump', 'landed', 'resumed', 'paused_again', 'stopped')
DETERMINISTIC = LABELS[1:9]
SHOTS = ('edit', 'grounded', 'walk', 'sprint', 'jump', 'landed', 'stopped',
         'river-restored', 'river-play')
VECTORS = ('position', 'velocity', 'direction')
COUNTERS = ('fixed_ticks', 'jumps_consumed', 'jumps_started')
BOOLS = ('walk_enabled', 'grounded', 'jump_pending', 'sprint')
FIELDS = ('label', 'authored_id', 'scene_id', 'generation', 'mode') + VECTORS + COUNTERS + BOOLS
SPAWN = [48, 126, 31]
SCENE_ID = 1317415599531854025  # scene::hash_authored_id: FNV-1a with high bit cleared


class EvidenceError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise EvidenceError(message)


def read_json(path):
    try:
        return json.loads(path.read_text(encoding='utf-8-sig'))
    except (OSError, ValueError) as error:
        raise EvidenceError(f'{path}: {error}') from error


def finite_number(value):
    return type(value) in (int, float) and math.isfinite(value)


def same_state(a, b, fields, tolerance=0):
    for key in fields:
        if key in VECTORS:
            require(all(abs(x - y) <= tolerance for x, y in zip(a[key], b[key])),
                    f'{a["label"]}/{b["label"]}: {key} drift exceeds {tolerance}')
        else:
            require(a[key] == b[key], f'{a["label"]}/{b["label"]}: {key} changed')


def parse_status(log):
    rows = {}
    for line in log.splitlines():
        if not line.startswith('character_status '):
            continue
        try:
            row = json.loads(line[len('character_status '):])
        except ValueError as error:
            raise EvidenceError(f'invalid character JSON: {error}') from error
        require(isinstance(row, dict), 'character status must be an object')
        for field in FIELDS:
            require(field in row, f'character status missing field {field}')
        label = row['label']
        require(isinstance(label, str) and label in LABELS, f'unexpected label {label!r}')
        require(label not in rows, f'duplicate label {label}')
        for key in VECTORS:
            value = row[key]
            require(isinstance(value, list) and len(value) == 3 and all(map(finite_number, value)),
                    f'{label}: {key} must contain three finite numbers')
        for key in COUNTERS + ('scene_id', 'generation'):
            require(type(row[key]) is int and 0 <= row[key] < 2**64,
                    f'{label}: {key} must be a uint64 integer')
        for key in BOOLS:
            require(type(row[key]) is bool, f'{label}: {key} must be boolean')
        require(row['authored_id'] == 'river-player' and row['scene_id'] == SCENE_ID,
                f'{label}: wrong authored scene identity')
        require(row['generation'] > 0, f'{label}: invalid generation')
        require(row['mode'] in ('edit', 'pause', 'play'), f'{label}: invalid mode')
        rows[label] = row
    require(tuple(rows) == LABELS, f'missing or misordered labels: got {tuple(rows)}')
    require(len({row['generation'] for row in rows.values()}) == 1, 'scene generation changed')
    return rows


def validate_run(root):
    root = Path(root).resolve()
    metadata = read_json(root / 'run.json')
    for key in ('started_ns', 'finished_ns', 'drive_exit', 'world'):
        require(key in metadata, f'run metadata missing field {key}')
    require(metadata['drive_exit'] == 0 and metadata['world'] == 'RiverFloatLab',
            'run did not exit successfully in RiverFloatLab')
    start, end = metadata['started_ns'], metadata['finished_ns']
    require(type(start) is int and type(end) is int and 0 < start < end, 'invalid run time interval')
    try:
        log = (root / 'log.txt').read_text(encoding='utf-8-sig')
    except OSError as error:
        raise EvidenceError(f'missing run log: {error}') from error
    bad = re.search(r'timeout|\bFATAL\b|Validation Error|VUID-|character: failed|'
                    r'cmd: unrecognized|dispatch failed|event:.*aborted|'
                    r'PhysX[^\n]*(?:disabled|fallback|unavailable)|'
                    r'(?:water|terrain|collision)[^\n]*(?:fallback|failed)|'
                    r'\[error\]|validation errors:\s*[1-9]', log, re.IGNORECASE)
    require(bad is None, f'forbidden error/timeout/fallback marker: {bad.group(0) if bad else ""}')
    for marker in ('viewer: bake ready', 'event: bake.finished', 'idle: settled after'):
        require(marker in log and log.index(marker) < log.find('character_status '),
                f'missing readiness marker before status: {marker}')
    installation = re.search(
        r'\[terrain-collision\] installed generation=([0-9a-f]+) geometry=([0-9a-f]+) '
        r'cell=0\.500 rung=2 regions=1 sectors=42 nonempty=(\d+) empty=(\d+) triangles=(\d+)', log)
    require(installation is not None and installation.start() < log.find('character_status '),
            'missing exact authored terrain collision installation before status')
    nonempty, empty, triangles = map(int, installation.groups()[2:])
    require(nonempty > 0 and nonempty + empty == 42 and triangles > 0,
            'incomplete/empty RiverFloatLab collision installation')
    rows = parse_status(log)
    # Walking's successful Ready transition is gated on configured collision install;
    # grounded telemetry therefore comes from the installed Box3D world, not a log guess.
    expected_ticks = dict(zip(LABELS[:9], (0, 300, 360, 390, 510, 510, 510, 511, 631)))
    expected_ticks['stopped'] = 0
    for label, row in rows.items():
        edit = label in ('edit', 'stopped')
        require(row['mode'] == ('edit' if edit else 'pause'), f'{label}: incorrect mode')
        require(row['walk_enabled'] == (not edit), f'{label}: incorrect walk ownership')
        require(all(lo <= value < hi for value, lo, hi in zip(row['position'], [-64]*3, [384, 128, 64])),
                f'{label}: outside unchanged collision union')
        if label in expected_ticks:
            require(row['fixed_ticks'] == expected_ticks[label], f'{label}: unexpected fixed_ticks {row["fixed_ticks"]}')
        jumped = label in ('jump', 'landed', 'resumed', 'paused_again')
        require(row['jumps_consumed'] == int(jumped) and row['jumps_started'] == int(jumped),
                f'{label}: jump consumption/launch count incorrect')
        require(row['jump_pending'] == (label in ('latched', 'paused')), f'{label}: incorrect jump latch')
        require(row['direction'] == ([1, 0, 0] if label in ('walk', 'sprint') else [0, 0, 0]),
                f'{label}: incorrect intent')
        require(row['sprint'] == (label == 'sprint'), f'{label}: incorrect sprint intent')
    for label in ('edit', 'stopped'):
        require(rows[label]['position'] == SPAWN and rows[label]['velocity'] == [0, 0, 0]
                and not rows[label]['grounded'], f'{label}: initial snapshot not restored')
    for label in ('grounded', 'jump_base', 'landed'):
        require(rows[label]['grounded'], f'{label}: not grounded on installed terrain')
    for before, after, max_distance in (('grounded', 'walk', 6), ('walk', 'sprint', 4.5)):
        a, b = rows[before]['position'], rows[after]['position']
        distance = math.hypot(b[0] - a[0], b[2] - a[2])
        require((.5 if after == 'walk' else 0) < distance <= max_distance and b[0] > a[0],
                f'{after}: missing forward progress or teleport ({distance} m)')
        require(abs(b[1] - a[1]) <= max_distance, f'{after}: vertical teleport')
    same_state(rows['latched'], rows['paused'], VECTORS + COUNTERS + BOOLS)
    same_state(rows['jump_base'], rows['latched'], VECTORS + COUNTERS + ('grounded',))
    require(not rows['jump']['grounded'] and rows['jump']['velocity'][1] > 0
            and 0 < rows['jump']['position'][1] - rows['jump_base']['position'][1] < .2,
            'jump: one step did not launch upward')
    require(rows['resumed']['fixed_ticks'] > 631, 'resume did not advance fixed ticks')
    same_state(rows['resumed'], rows['paused_again'], VECTORS + COUNTERS + BOOLS)
    for label in SHOTS:
        png = root / (label + '.png')
        done = root / (label + '.png.done')
        for artifact in (png, done):
            require(artifact.is_file() and start <= artifact.stat().st_mtime_ns <= end,
                    f'missing/stale capture: {artifact}')
        require(done.read_text().strip() == 'captured', f'incomplete capture marker: {done}')
        data = png.read_bytes()
        require(len(data) > 33 and data[:8] == b'\x89PNG\r\n\x1a\n'
                and data[12:16] == b'IHDR' and struct.unpack('>II', data[16:24]) == (1280, 720),
                f'invalid/wrong-size PNG: {png}')
        require(f'screenshot written to {png.as_posix()}' in log.replace('\\', '/'),
                f'capture missing current write acknowledgement: {png}')
    return rows


def compare_runs(first, second):
    for label in DETERMINISTIC:
        same_state(first[label], second[label], VECTORS + COUNTERS + BOOLS +
                   ('scene_id', 'generation', 'mode', 'authored_id'), tolerance=.001)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', action='append', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    try:
        require(len(args.run) == 2, 'exactly two fresh-process runs required')
        require(args.run[0].resolve() != args.run[1].resolve(), 'run paths must differ')
        runs = [validate_run(root) for root in args.run]
        compare_runs(*runs)
        result = dict(status='pass', tolerance_m=.001, runs=runs,
                      limitations='No whole-river traversal, swimming, craft riding, or buoyancy endurance proof')
        code = 0
    except (EvidenceError, OSError, ValueError) as error:
        result = dict(status='fail', error=str(error))
        code = 1
    args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(dict(status=result['status'], output=str(args.output), error=result.get('error'))))
    return code


if __name__ == '__main__':
    sys.exit(main())
