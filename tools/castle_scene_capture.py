#!/usr/bin/env python3
"""Capture one closed-world castle only after full publish_pipeline return.

Native Windows Python:
  py -3 tools/castle_scene_capture.py --world CastleSiteGallery \
      --editor MatterEditor/build/windows-msvc/editor.exe --timeline cameras.txt \
      --out-dir D:/tmp/castle-capture --timeout 3600

Read-only gate for an editor already owned by another driver:
  python tools/castle_scene_capture.py --follow-log /tmp/run/log.txt \
      --log-offset 0 --timeout 3600

The log offset must identify the start of the CURRENT single-world run. This
helper does not infer generations from old logs. Launch mode creates a fresh
empty FIFO and log, waits for numeric final bake timing + clean BakeFinished +
viewer readiness, then sends ONLY wait_idle. Camera/shot commands are appended
only after idle succeeds. A timeout never releases the screenshot timeline.
Warm resolve-cache placeholders (publish=...ms) do not satisfy the barrier.
Streamed worlds are rejected: roots-only timing does not prove sector fill.

This is a conservative diagnostic-log barrier, not a new engine event. The
initial BakeFinished is already after the FIFO GPU finalize barrier; deferred
tilesets and RefineController setup precede the numeric timing record. Missing
geometry AFTER this barrier still needs renderer/content investigation.
"""
import argparse
import json
import os
from pathlib import Path
import re
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
FINAL_TIMING = re.compile(r'\[bake-timing\]\s+install=([\d.]+)ms\s+compose=([\d.]+)ms\s+'
                          r'publish=([\d.]+)ms\s+total=([\d.]+)ms(?:\s+\(resolve-cache-hit\))?')
FAILURE = re.compile(r'\bFATAL\b|\bVUID-|Validation Error|\bbake error\b|\bbake aborted\b|'
                     r'flatten failed|validation errors:\s*[1-9]|\[error\]|'
                     r'cmd: unrecognized|dispatch failed|shot: timeout|idle: timeout|'
                     r'event:.*(?:timeout|aborted)|\bassertion.*failed', re.I)


class CaptureError(RuntimeError):
    pass


class PublicationGate:
    """Feed only fresh log lines from ONE closed-world process/generation."""
    def __init__(self):
        self.timing = None
        self.finished = False
        self.viewer_ready = False
        self.idle = False
        self.error = None
        self.lines = 0
        self.acknowledged_shots = set()

    def feed(self, line):
        self.lines += 1
        if FAILURE.search(line):
            self.error = line.strip()
        if '[bake-timing]' in line and ('world-kind' in line or 'ROOTS ONLY' in line):
            self.error = 'streamed world: roots-only timing cannot prove complete geometry; use a sector-fill-aware driver'
        finished = re.search(r'\bbake finished \((\d+) errors\)', line)
        if finished:
            if int(finished[1]):
                self.error = line.strip()
            else:
                self.finished = True
        if 'viewer: bake ready' in line:
            self.viewer_ready = True
        match = FINAL_TIMING.search(line)
        if match:
            self.timing = dict(zip(('install_ms', 'compose_ms', 'publish_ms', 'total_ms'),
                                   map(float, match.groups())))
            self.timing['resolve_cache_hit'] = '(resolve-cache-hit)' in line
        if 'idle: settled after ' in line:
            self.idle = True
        shot = re.search(r'screenshot written to (.+)', line)
        if shot:
            self.acknowledged_shots.add(shot[1].strip().replace('\\', '/'))

    @property
    def published(self):
        return self.error is None and self.timing is not None and self.finished and self.viewer_ready

    def missing(self):
        return [name for name, value in [('numeric final bake timing', self.timing),
                ('clean bake finished', self.finished), ('viewer ready', self.viewer_ready)] if not value]

    def receipt(self):
        return {'published': self.published, 'timing': self.timing, 'lines_read': self.lines,
                'idle_confirmed': self.idle, 'error': self.error, 'missing': self.missing()}


def await_gate(condition, gate, predicate, deadline, alive, phase):
    with condition:
        while True:
            if gate.error:
                raise CaptureError(gate.error)
            if predicate():
                return
            if not alive():
                raise CaptureError(f'editor/log reader exited during {phase}; missing {gate.missing()}')
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise CaptureError(f'timeout during {phase}; missing {gate.missing()}')
            condition.wait(min(.25, remaining))


def follow_log(path, offset, timeout):
    if offset is None or offset < 0:
        raise CaptureError('--follow-log requires explicit --log-offset at the current run start (0 for a fresh log)')
    gate = PublicationGate()
    deadline = time.monotonic() + timeout
    pending = b''
    with Path(path).open('rb') as stream:
        if os.fstat(stream.fileno()).st_size < offset:
            raise CaptureError('log is shorter than the supplied run offset')
        stream.seek(offset)
        identity = os.fstat(stream.fileno())
        while True:
            current = Path(path).stat()
            if (current.st_dev, current.st_ino) != (identity.st_dev, identity.st_ino):
                raise CaptureError('log was replaced during publication wait')
            if os.fstat(stream.fileno()).st_size < stream.tell():
                raise CaptureError('log was truncated/replaced during publication wait')
            chunk = stream.read()
            pending += chunk
            lines = pending.split(b'\n')
            pending = lines.pop()
            for line in lines:
                gate.feed(line.decode('utf-8', errors='replace'))
            if gate.error:
                raise CaptureError(gate.error)
            if gate.published:
                return gate.receipt()
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise CaptureError('publication timeout; missing ' + ', '.join(gate.missing()))
            threading.Event().wait(min(.2, remaining))


def timeline_commands(path):
    commands = []
    shots = []
    for raw in Path(path).read_text(encoding='utf-8').splitlines():
        line = raw.strip()
        if not line or line.startswith('#'):
            continue
        verb = line.split()[0]
        if verb in {'world', 'regen', 'regenerate', 'reload', 'open_world', 'open_project'}:
            raise CaptureError('capture timeline must stay in one bake generation: ' + line)
        if verb in {'wait_event', 'wait_idle'}:
            raise CaptureError('remove wait_event/wait_idle from timeline; the helper owns publication and idle waits')
        if verb in {'shot', 'shot_now'}:
            fields = line.split()
            if len(fields) != 2 or not fields[1].lower().endswith('.png'):
                raise CaptureError('shot path must be one whitespace-free PNG path: ' + line)
            shots.append(fields[1])
        commands.append(line)
    if not shots:
        raise CaptureError('timeline contains no screenshots')
    if commands[-1] != 'quit' or any(c == 'quit' for c in commands[:-1]):
        raise CaptureError('single-world timeline must end with exactly one quit')
    return commands, shots


def verify_shots(shots, editor_dir, gate):
    for text in shots:
        path = Path(text)
        if not path.is_absolute():
            path = editor_dir / path
        data = path.read_bytes()
        if len(data) < 24 or data[:8] != b'\x89PNG\r\n\x1a\n' or data[12:16] != b'IHDR':
            raise CaptureError('invalid PNG: ' + str(path))
        if not all(struct.unpack('>II', data[16:24])):
            raise CaptureError('empty PNG: ' + str(path))
        if path.with_name(path.name + '.done').read_text().strip() != 'captured':
            raise CaptureError('missing screenshot completion: ' + str(path))
        if text.replace('\\', '/') not in gate.acknowledged_shots and path.as_posix() not in gate.acknowledged_shots:
            raise CaptureError('missing current-process screenshot acknowledgment: ' + str(path))


def stop_owned_process(proc):
    if proc.poll() is not None:
        return
    if os.name == 'nt':
        subprocess.run(['taskkill', '/F', '/T', '/PID', str(proc.pid)], check=False,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    else:
        os.killpg(proc.pid, signal.SIGKILL)
    proc.wait(timeout=15)


def capture(args):
    commands, shots = timeline_commands(args.timeline)
    editor = Path(args.editor).resolve()
    editor_dir = Path(args.editor_dir).resolve()
    out = Path(args.out_dir).resolve()
    out.mkdir(parents=True, exist_ok=True)
    for previous in ('capture.json', 'capture-failure.json'):
        (out / previous).unlink(missing_ok=True)
    fifo = out / 'cmd.txt'
    fifo.write_text('', encoding='utf-8')
    for text in shots:
        path = Path(text)
        if not path.is_absolute():
            path = editor_dir / path
        for stale in (path, path.with_name(path.name + '.done')):
            stale.unlink(missing_ok=True)
    # Clear inherited automation so a stale screenshot/camera timeline cannot
    # race this driver's publication barrier. Explicit overrides remain allowed.
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith('MATTER_')}
    env['MATTER_HIDE_WINDOW'] = '1'
    for item in args.env:
        if '=' not in item:
            raise CaptureError('--env requires K=V')
        k, v = item.split('=', 1)
        if k.upper() in {'MATTER_CMD_FIFO', 'MATTER_WORLD', 'MATTER_SCREENSHOT', 'MATTER_CAM_PATH'}:
            raise CaptureError('automation environment is owned by this helper: ' + k)
        env[k] = v
    env.update(MATTER_WORLD=args.world, MATTER_CMD_FIFO=str(fifo), MATTER_HIDE_UI='1',
               TMP=tempfile.gettempdir(), TEMP=tempfile.gettempdir())
    gate = PublicationGate()
    condition = threading.Condition()
    reader_done = False
    deadline = time.monotonic() + args.timeout
    proc = subprocess.Popen([str(editor)], cwd=editor_dir, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding='utf-8', errors='replace', bufsize=1,
                            start_new_session=os.name != 'nt')

    def read_output():
        nonlocal reader_done
        try:
            with (out / 'log.txt').open('w', encoding='utf-8') as log:
                for line in proc.stdout:
                    log.write(line)
                    log.flush()
                    with condition:
                        gate.feed(line)
                        condition.notify_all()
        except Exception as error:
            with condition:
                gate.error = 'log reader failed: ' + str(error)
        finally:
            with condition:
                reader_done = True
                condition.notify_all()

    reader = threading.Thread(target=read_output, daemon=True)
    reader.start()
    print(f'capture: editor pid={proc.pid}; awaiting final publication; log={out / "log.txt"}', flush=True)

    def append(lines):
        with fifo.open('a', encoding='utf-8', newline='\n') as stream:
            stream.write('\n'.join(lines) + '\n')
            stream.flush()

    try:
        await_gate(condition, gate, lambda: gate.published, deadline,
                   lambda: not reader_done, 'complete geometry publication')
        print('capture: publication complete ' + json.dumps(gate.timing), flush=True)
        with condition:
            gate.idle = False
        remaining = max(.001, deadline - time.monotonic())
        append([f'wait_idle {args.settle_seconds:g} {remaining:.3f}'])
        await_gate(condition, gate, lambda: gate.idle, deadline,
                   lambda: not reader_done, 'post-publication idle acknowledgment')
        append(['wait_frames 2', *commands])
        print('capture: idle confirmed; screenshot timeline submitted', flush=True)
        await_gate(condition, gate, lambda: reader_done, deadline,
                   lambda: True, 'screenshot timeline completion')
        code = proc.wait(timeout=max(.001, deadline - time.monotonic()))
        if code:
            raise CaptureError('editor exited with code ' + str(code))
        verify_shots(shots, editor_dir, gate)
        receipt = gate.receipt()
        receipt.update(world=args.world, shots=shots, editor_exit=code)
        (out / 'capture.json').write_text(json.dumps(receipt, indent=2)+'\n')
        return receipt
    except Exception as error:
        with condition:
            gate.error = gate.error or str(error)
        raise
    finally:
        stop_owned_process(proc)
        reader.join(timeout=5)
        if not (out / 'capture.json').exists():
            (out / 'capture-failure.json').write_text(json.dumps(gate.receipt(), indent=2)+'\n')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--follow-log', type=Path)
    parser.add_argument('--log-offset', type=int)
    parser.add_argument('--world')
    parser.add_argument('--editor', default=ROOT / 'MatterEditor/build/windows-msvc/editor.exe')
    parser.add_argument('--editor-dir', default=ROOT / 'MatterEditor')
    parser.add_argument('--timeline', type=Path)
    parser.add_argument('--out-dir', type=Path)
    parser.add_argument('--timeout', type=float, default=3600)
    parser.add_argument('--settle-seconds', type=float, default=3)
    parser.add_argument('--env', action='append', default=[])
    args = parser.parse_args(argv)
    try:
        if not (0 < args.timeout < float('inf')) or not (0 < args.settle_seconds < float('inf')):
            raise CaptureError('timeouts and settle seconds must be finite and positive')
        if args.follow_log:
            result = follow_log(args.follow_log, args.log_offset, args.timeout)
        else:
            if not all((args.world, args.timeline, args.out_dir)):
                raise CaptureError('launch mode requires --world, --timeline, and --out-dir')
            if sys.platform in {'msys', 'cygwin'}:
                raise CaptureError('use native Windows Python for Windows editor paths')
            result = capture(args)
        print(json.dumps(result, indent=2))
        return 0
    except (CaptureError, OSError, subprocess.TimeoutExpired) as error:
        print('capture: FAIL: ' + str(error), file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
