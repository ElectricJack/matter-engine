import importlib.util
from pathlib import Path
import tempfile
import threading
import time
import unittest
from unittest.mock import patch
from types import SimpleNamespace
import struct

SPEC = importlib.util.spec_from_file_location('capture', Path(__file__).resolve().parents[1] / 'castle_scene_capture.py')
M = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(M)


class PublicationTests(unittest.TestCase):
    def ready(self, gate):
        gate.feed('bake finished (0 errors)\n')
        gate.feed('viewer: bake ready\n')

    def test_finished_and_false_idle_do_not_publish(self):
        gate = M.PublicationGate()
        self.ready(gate)
        gate.feed('idle: settled after 3.0s\n')
        self.assertFalse(gate.published)
        gate.feed('[bake-timing] install=110700ms compose=2ms publish=1008500ms total=1119202ms\n')
        self.assertTrue(gate.published)

    def test_cache_placeholder_not_a_barrier_and_event_order_independent(self):
        gate = M.PublicationGate()
        gate.feed('[bake-timing] install=0ms compose=0ms (resolve-cache-hit) publish=...ms total(pre-publish)=1ms\n')
        self.ready(gate)
        self.assertFalse(gate.published)
        gate.feed('[bake-timing] install=0ms compose=0ms publish=312ms total=313ms (resolve-cache-hit)\n')
        self.assertTrue(gate.published)
        self.assertTrue(gate.timing['resolve_cache_hit'])
        other = M.PublicationGate()
        other.feed('[bake-timing] install=1ms compose=2ms publish=3ms total=6ms\n')
        self.assertFalse(other.published)
        self.ready(other)
        self.assertTrue(other.published)

    def test_fail_closed_after_apparently_successful_event(self):
        for line in ['bake finished (2 errors)', 'idle: timeout after 30s',
                     'event: bake.finished timeout after 30s', 'bake aborted [parts]: cancelled',
                     '[bake-timing] install=1ms world=2ms publish=3ms total=6ms (world-kind: ROOTS ONLY)']:
            gate = M.PublicationGate()
            self.ready(gate)
            gate.feed(line)
            self.assertIsNotNone(gate.error, line)
            with self.assertRaises(M.CaptureError):
                M.await_gate(threading.Condition(), gate, lambda: True, time.monotonic()+1, lambda: True, 'test')

    def test_timeout_and_early_process_exit(self):
        for alive, deadline, message in [(True, time.monotonic()-1, 'timeout'), (False, time.monotonic()+1, 'exited')]:
            with self.assertRaisesRegex(M.CaptureError, message):
                M.await_gate(threading.Condition(), M.PublicationGate(), lambda: False,
                             deadline, lambda: alive, 'publication')

    def test_follow_log_current_run_offset_and_warm_path(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)/'log.txt'
            old = b'bake finished (0 errors)\nviewer: bake ready\n[bake-timing] install=1ms compose=2ms publish=3ms total=6ms\n'
            path.write_bytes(old)
            with self.assertRaisesRegex(M.CaptureError, 'timeout'):
                M.follow_log(path, len(old), .01)
            path.write_bytes(old+b'bake finished (0 errors)\nviewer: bake ready\n[bake-timing] install=0ms compose=0ms publish=3ms total=3ms (resolve-cache-hit)\n')
            result = M.follow_log(path, len(old), .01)
            self.assertTrue(result['published'])
            self.assertTrue(result['timing']['resolve_cache_hit'])
            with self.assertRaisesRegex(M.CaptureError, 'explicit'):
                M.follow_log(path, None, .01)

    def test_driver_defers_shots_and_does_not_continue_after_idle_timeout(self):
        # A CPU-only fake process drives the real threaded log/FIFO protocol.
        # No editor, GPU or native executable is launched.
        for idle_timeout in (False, True):
            with tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                image = root/'view.png'
                timeline = root/'timeline.txt'
                timeline.write_text(f'shot_now {image.as_posix()}\nquit\n')
                output = root/'run'
                output.mkdir()
                (output/'capture.json').write_text('STALE')
                args = SimpleNamespace(timeline=timeline, editor=root/'fake-editor',
                    editor_dir=root, out_dir=output, env=[], world='FakeCastle',
                    timeout=3, settle_seconds=.01)

                class FakeProcess:
                    def __init__(self, *unused, **options):
                        self.returncode = None
                        self.pid = 0  # Fake process: never passed to an OS kill call.
                        self.stdout = self.generate()

                    def generate(self):
                        fifo = output/'cmd.txt'
                        yield 'bake finished (0 errors)\n'
                        yield 'viewer: bake ready\n'
                        yield 'idle: settled after 3s\n'
                        yield '[bake-timing] install=0ms compose=0ms (resolve-cache-hit) publish=...ms total(pre-publish)=1ms\n'
                        threading.Event().wait(.025)
                        assert fifo.read_text() == '', 'commands escaped before final publication'
                        yield '[bake-timing] install=0ms compose=0ms publish=42ms total=43ms (resolve-cache-hit)\n'
                        deadline = time.monotonic()+1
                        while not fifo.read_text() and time.monotonic()<deadline:
                            threading.Event().wait(.005)
                        assert fifo.read_text().startswith('wait_idle ')
                        assert 'shot' not in fifo.read_text(), 'capture escaped before idle acknowledgment'
                        if idle_timeout:
                            yield 'idle: timeout after 1s\n'
                            self.returncode = 0
                            return
                        yield 'idle: settled after .01s\n'
                        while 'shot_now' not in fifo.read_text() and time.monotonic()<deadline:
                            threading.Event().wait(.005)
                        assert 'shot_now' in fifo.read_text()
                        image.write_bytes(b'\x89PNG\r\n\x1a\n'+b'\0'*4+b'IHDR'+struct.pack('>II',1,1))
                        image.with_name(image.name+'.done').write_text('captured')
                        yield f'screenshot written to {image.as_posix()}\n'
                        self.returncode = 0

                    def poll(self):
                        return self.returncode

                    def wait(self, timeout=None):
                        return self.returncode

                with patch.object(M.subprocess, 'Popen', FakeProcess), patch.object(M, 'stop_owned_process'):
                    if idle_timeout:
                        with self.assertRaisesRegex(M.CaptureError, 'idle: timeout'):
                            M.capture(args)
                        self.assertNotIn('shot_now', (output/'cmd.txt').read_text())
                        self.assertFalse((output/'capture.json').exists())
                        self.assertIn('idle: timeout', (output/'capture-failure.json').read_text())
                    else:
                        result = M.capture(args)
                        self.assertTrue(result['published'])
                        self.assertTrue(result['idle_confirmed'])
                        self.assertTrue((output/'capture.json').exists())

    def test_timelines_cannot_own_unsafe_waits_or_change_world(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)/'timeline.txt'
            for first in ['world Other', 'wait_event bake.finished 10', 'wait_idle 3 10']:
                path.write_text(first+'\nshot_now D:/tmp/fresh.png\nquit\n')
                with self.assertRaises(M.CaptureError):
                    M.timeline_commands(path)
            path.write_text('cam 0 1 2 0 0 0\nshot_now D:/tmp/fresh.png\nquit\n')
            self.assertEqual(M.timeline_commands(path)[1], ['D:/tmp/fresh.png'])


if __name__ == '__main__':
    unittest.main()
