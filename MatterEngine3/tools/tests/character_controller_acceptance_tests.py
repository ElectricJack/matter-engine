"""Behavioral rejection tests for the retained character acceptance evidence."""
import copy
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import time
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import character_controller_acceptance as checker

LABELS = ('edit', 'grounded', 'walk', 'sprint', 'jump_base', 'latched',
          'paused', 'jump', 'landed', 'resumed', 'paused_again', 'stopped')
SHOTS = ('edit', 'grounded', 'walk', 'sprint', 'jump', 'landed', 'stopped',
         'river-restored', 'river-play')


def valid_trace():
    ticks = (0, 300, 360, 390, 510, 510, 510, 511, 631, 650, 650, 0)
    positions = ([48, 126, 31], [48, 91, 31], [52, 91, 31],
                 [55, 91, 31], [55, 91, 31], [55, 91, 31], [55, 91, 31],
                 [55, 91.08, 31], [55, 91, 31], [55, 91, 31],
                 [55, 91, 31], [48, 126, 31])
    rows = []
    for i, label in enumerate(LABELS):
        rows.append(dict(label=label, authored_id='river-player',
                         scene_id=1317415599531854025, generation=1,
                         mode='edit' if label in ('edit', 'stopped') else 'pause',
                         walk_enabled=label not in ('edit', 'stopped'),
                         position=positions[i], velocity=[0, 4.8365, 0] if label == 'jump' else [0, 0, 0],
                         grounded=label not in ('edit', 'jump', 'stopped'),
                         fixed_ticks=ticks[i], jumps_consumed=int(7 <= i <= 10),
                         jumps_started=int(7 <= i <= 10),
                         jump_pending=label in ('latched', 'paused'),
                         direction=[1, 0, 0] if label in ('walk', 'sprint') else [0, 0, 0],
                         sprint=label == 'sprint'))
    return rows


def png_bytes():
    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 1280, 720, 8, 2, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress((b'\0' + b'\0' * (1280 * 3)) * 720)) + chunk(b'IEND', b''))


class AcceptanceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.rows = valid_trace()

    def write_run(self, name='run1', rows=None, extra=''):
        root = self.root / name
        root.mkdir(exist_ok=True)
        started = time.time_ns() - 1_000_000_000
        log = ('[terrain-collision] installed generation=718bb26177c7e3d4 '
               'geometry=382bf7bdfe5930ca cell=0.500 rung=2 regions=1 sectors=42 '
               'nonempty=20 empty=22 triangles=793365 vertices=402449\n'
               'viewer: bake ready\nevent: bake.finished\nidle: settled after 120.0s\n') + extra
        log += ''.join('character_status ' + json.dumps(row) + '\n' for row in (rows or self.rows))
        for shot in SHOTS:
            path = root / (shot + '.png')
            path.write_bytes(png_bytes())
            path.with_suffix('.png.done').write_text('captured\n')
            log += f'screenshot written to {path.as_posix()}\n'
        (root / 'log.txt').write_text(log)
        (root / 'run.json').write_text(json.dumps(dict(started_ns=started,
            finished_ns=time.time_ns() + 1_000_000_000, drive_exit=0, world='RiverFloatLab')))
        return root

    def reject_mutation(self, label, key, value):
        rows = copy.deepcopy(self.rows)
        next(row for row in rows if row['label'] == label)[key] = value
        with self.assertRaises(checker.EvidenceError):
            checker.validate_run(self.write_run(rows=rows))

    def test_complete_trace_and_current_pngs_pass(self):
        result = checker.validate_run(self.write_run())
        self.assertEqual(result['walk']['fixed_ticks'], 360)

    def test_missing_and_duplicate_labels_fail(self):
        for rows in (self.rows[:-1], self.rows + [self.rows[0]]):
            with self.subTest(rows=len(rows)), self.assertRaises(checker.EvidenceError):
                checker.validate_run(self.write_run(rows=rows))

    def test_nonfinite_and_wrong_numeric_types_fail(self):
        for value in (float('nan'), float('inf'), '48', True):
            with self.subTest(value=value):
                self.reject_mutation('walk', 'position', [value, 91, 31])

    def test_missing_field_is_explicit(self):
        del self.rows[1]['velocity']
        with self.assertRaisesRegex(checker.EvidenceError, 'missing.*velocity'):
            checker.validate_run(self.write_run())

    def test_wrong_authored_identity_or_generation_fail(self):
        for key, value in (('authored_id', 'other'), ('scene_id', 5),
                           ('scene_id', 10540787636386629833), ('generation', 2)):
            self.reject_mutation('walk', key, value)

    def test_unexpected_fixed_counts_fail(self):
        self.reject_mutation('walk', 'fixed_ticks', 361)

    def test_double_consumed_jump_fails(self):
        self.reject_mutation('landed', 'jumps_consumed', 2)

    def test_airborne_press_counted_as_launch_fails(self):
        self.reject_mutation('landed', 'jumps_started', 2)

    def test_paused_drift_and_consumption_fail(self):
        self.reject_mutation('paused', 'position', [55, 91.1, 31])
        self.reject_mutation('paused', 'jump_pending', False)

    def test_overwritten_stop_snapshot_fails(self):
        self.reject_mutation('stopped', 'position', [55, 91, 31])

    def test_unsupported_no_progress_and_teleport_fail(self):
        self.reject_mutation('grounded', 'grounded', False)
        self.reject_mutation('walk', 'position', [48, 91, 31])
        self.reject_mutation('walk', 'position', [200, 91, 31])

    def test_cross_run_drift_tolerance(self):
        one = checker.validate_run(self.write_run())
        two = copy.deepcopy(one)
        two['walk']['position'][0] += .0005
        checker.compare_runs(one, two)
        two['walk']['position'][0] += .001
        with self.assertRaises(checker.EvidenceError):
            checker.compare_runs(one, two)

    def test_missing_stale_and_empty_capture_fail(self):
        for kind in ('missing', 'stale', 'empty'):
            root = self.write_run(name=kind)
            path = root / 'walk.png.done'
            if kind == 'missing':
                path.unlink()
            elif kind == 'stale':
                os.utime(path, ns=(1, 1))
            else:
                path.write_text('')
            with self.subTest(kind=kind), self.assertRaises(checker.EvidenceError):
                checker.validate_run(root)

    def test_timeout_error_and_physx_fallback_fail(self):
        for marker in ('event: bake.finished timeout after 3600s',
                       'idle: timeout after 120s', 'shot: timeout, abandoned x',
                       'character: failed player missing', 'cmd: unrecognized x',
                       'FATAL: broken', 'Validation Error: VUID-123',
                       'PhysX fluid support is disabled in this build',
                       'PhysX fallback active', 'water animation static fallback'):
            with self.subTest(marker=marker), self.assertRaises(checker.EvidenceError):
                checker.validate_run(self.write_run(extra=marker + '\n'))

    def test_missing_or_incomplete_collision_install_fails(self):
        for replacement in ('', 'nonempty=0 empty=42 triangles=0'):
            root = self.write_run()
            log = (root / 'log.txt').read_text()
            if replacement:
                log = log.replace('nonempty=20 empty=22 triangles=793365', replacement)
            else:
                log = '\n'.join(log.splitlines()[1:])
            (root / 'log.txt').write_text(log)
            with self.subTest(replacement=replacement), self.assertRaises(checker.EvidenceError):
                checker.validate_run(root)


if __name__ == '__main__':
    unittest.main()
