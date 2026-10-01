#!/usr/bin/env python3
"""Serial restored-population captures using the existing attribution driver."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument('--root', default='/mnt/c/tmp/quick-meadow68-4-20260930')
args = parser.parse_args()
root = Path(args.root)
root.mkdir(parents=True, exist_ok=True)
repo = Path(__file__).resolve().parents[4]
exe = repo / 'MatterEditor/build/windows-msvc/editor.exe'
source = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=repo, text=True).strip()
binary = hashlib.sha256(exe.read_bytes()).hexdigest()
protocol = dict(source_sha=source, editor_sha256=binary, camera=dict(
    eye=[380, 90, 1600], target=[420, 55, 1420]), width=1920, height=1080,
    sample_seconds=20, runs=[])
protocol['input_sha256'] = {
    name: hashlib.sha256((repo / name).read_bytes()).hexdigest()
    for name in ('projects/world_demo/scenes/streaming/StreamMountain/StreamMountain.js',
                 'projects/world_demo/scenes/streaming/StreamMountain/props.json',
                 'projects/world_demo/scenes/streaming/StreamMountain/objects/WorldSector.js',
                 'projects/world_demo/shared-lib/mountain_geometry_site.js')}
(root / 'editor-candidate.exe').write_bytes(exe.read_bytes())
for warmup in (45, 300):
    for setup in ('static', 'vg'):
        out = root / f'{setup}-w{warmup}'
        if out.exists():
            raise RuntimeError(f'Refusing to overwrite {out}')
        env = os.environ.copy()
        env.update(PAGED_TERRAIN=str(int(setup == 'vg')), PAGED_CACHE_ONLY='0',
                   VARIANTS='pom_off', RUNS='1', WARMUP=str(warmup))
        windows_out = subprocess.check_output(['wslpath', '-m', str(out)], text=True).strip()
        command = ['tools/streammountain_attribution.sh', windows_out]
        run = dict(setup=setup, warmup_seconds=warmup, command=command,
                   overrides={k: env[k] for k in ('PAGED_TERRAIN', 'PAGED_CACHE_ONLY',
                              'VARIANTS', 'RUNS', 'WARMUP')}, started_at=time.time())
        print(time.strftime('%Y-%m-%dT%H:%M:%S%z'), 'START', out.name, flush=True)
        with (root / f'{out.name}.driver.log').open('w') as log:
            process = subprocess.Popen(command, cwd=repo, env=env,
                                       stdout=log, stderr=subprocess.STDOUT)
            captured = False
            warmup_seen_at = None
            while process.poll() is None:
                with (root / 'host-load.csv').open('a') as load_log:
                    load_log.write(f'{time.time()},{out.name},' + Path('/proc/loadavg').read_text())
                log_path = out / 'pom_off.log'
                try:
                    text = log_path.read_text(errors='replace')
                except OSError:
                    text = ''
                # Capture during wall-clock warmup, outside the timed sample.
                if warmup_seen_at is None and 'warming for' in text:
                    warmup_seen_at = time.monotonic()
                shot_delay = 20 if warmup == 45 else 180
                if (not captured and warmup_seen_at is not None
                        and time.monotonic() - warmup_seen_at >= shot_delay):
                    shot = windows_out + '/view.png'
                    with (out / 'pom_off.commands.txt').open('a') as commands:
                        commands.write('stats candidate-view\nshot_now ' + shot + '\n')
                    captured = True
                    run['shot_requested_at'] = time.time()
                    run['shot_delay_from_warmup_marker_seconds'] = time.monotonic() - warmup_seen_at
                time.sleep(2)
            run['exit_code'] = process.wait()
        run['finished_at'] = time.time()
        marker = out / 'view.png.done'
        run['screenshot_captured'] = marker.is_file() and marker.read_text().strip() == 'captured'
        protocol['runs'].append(run)
        (root / 'protocol.json').write_text(json.dumps(protocol, indent=2) + '\n')
        print('FINISH', out.name, 'exit', run['exit_code'],
              'screenshot', run['screenshot_captured'], flush=True)
        if run['exit_code'] or not run['screenshot_captured']:
            raise SystemExit(1)
