"""Run the real vulkan_smoke_tests RT counter modes on the Linux device.

MATTER_SMOKE_EXE=<built vulkan_smoke_tests> MATTER_SMOKE_LOG_DIR=<new dir> \
MATTER_VK_LAYER_DIR=<dir holding VkLayer_khronos_validation.json> \
MATTER_VK_LAYER_LIB=<dir holding libVkLayer_khronos_validation.so> \
aq test -n 0 <this-file> -q -s

rt-counters is the counter-lifecycle regression matrix; rt runs the unchanged
invalid-record surface-query fixture. Each run is wrapped in /usr/bin/time -v
so peak RSS and wall time are recorded next to the full output.
"""
import os
from pathlib import Path
import re
import subprocess

import pytest

EXE = Path(os.environ['MATTER_SMOKE_EXE'])
LOGS = Path(os.environ['MATTER_SMOKE_LOG_DIR'])


def run_mode(mode):
    env = dict(os.environ, MATTER_VK_SMOKE_MODE=mode, DISPLAY=os.environ.get('DISPLAY', ':0'),
               VK_LAYER_PATH=os.environ['MATTER_VK_LAYER_DIR'])
    env['LD_LIBRARY_PATH'] = os.environ['MATTER_VK_LAYER_LIB'] + ':' + env.get('LD_LIBRARY_PATH', '')
    LOGS.mkdir(parents=True, exist_ok=True)
    log = LOGS / f'{mode}.log'
    with log.open('x') as out:
        done = subprocess.run(['/usr/bin/time', '-v', str(EXE)], env=env, cwd=EXE.parent,
                              stdout=out, stderr=subprocess.STDOUT, timeout=1500)
    text = log.read_text(errors='replace')
    rss = re.search(r'Maximum resident set size \(kbytes\): (\d+)', text)
    wall = re.search(r'Elapsed \(wall clock\) time.*: (\S+)', text)
    banner = [l for l in text.splitlines() if l.startswith('Vulkan RT available=')]
    fails = [l for l in text.splitlines() if 'FAIL' in l]
    validation = re.findall(r'validation errors: (\d+)', text)
    print(f'\n[{mode}] exit={done.returncode} rss_kb={rss and rss.group(1)} '
          f'wall={wall and wall.group(1)} validation={validation} banner={banner}')
    for line in fails[:40]:
        print(f'[{mode}] {line}')
    return done.returncode, text, validation


@pytest.mark.parametrize('mode', ['rt-counters', 'rt'])
def test_mode(mode):
    code, text, validation = run_mode(mode)
    assert code == 0, f'{mode} exited {code}'
    assert validation and all(v == '0' for v in validation), validation
