"""Run the real vulkan_smoke_tests RT counter modes on the Linux device.

MATTER_SMOKE_EXE=<built vulkan_smoke_tests> MATTER_SMOKE_LOG_DIR=<new dir> \
MATTER_VK_LAYER_DIR=<dir holding VkLayer_khronos_validation.json> \
MATTER_VK_LAYER_LIB=<dir holding libVkLayer_khronos_validation.so> \
aq test -n 0 <this-file> -q -s

Epoch 6: rt-counters only, hard 600 s cap, with MATTER_VK_END_FRAME_TRACE=1 so
every begin/end_frame sub-phase logs a flushed, timestamped line with fence
states, and end_frame observes the frame fence in 5 s slices. Each run is wrapped in /usr/bin/time -v
so peak RSS and wall time are recorded next to the full output.
"""
import os
from pathlib import Path
import re
import signal
import subprocess

import pytest

EXE = Path(os.environ['MATTER_SMOKE_EXE'])
LOGS = Path(os.environ['MATTER_SMOKE_LOG_DIR'])


def run_mode(mode):
    env = dict(os.environ, MATTER_VK_SMOKE_MODE=mode, DISPLAY=os.environ.get('DISPLAY', ':0'),
               VK_LAYER_PATH=os.environ['MATTER_VK_LAYER_DIR'],
               MATTER_VK_END_FRAME_TRACE='1')
    env['LD_LIBRARY_PATH'] = os.environ['MATTER_VK_LAYER_LIB'] + ':' + env.get('LD_LIBRARY_PATH', '')
    LOGS.mkdir(parents=True, exist_ok=True)
    log = LOGS / f'{mode}.log'
    with log.open('x') as out:
        # Contain the timed executable as well as /usr/bin/time. Killing only
        # the direct child on timeout leaves the Vulkan process running.
        with subprocess.Popen(['/usr/bin/time', '-v', 'stdbuf', '-oL', '-eL', str(EXE)],
                              env=env, cwd=EXE.parent, stdout=out,
                              stderr=subprocess.STDOUT, start_new_session=True) as child:
            try:
                child.wait(timeout=600)
            finally:
                if child.poll() is None:
                    os.killpg(child.pid, signal.SIGTERM)
                    try:
                        child.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        os.killpg(child.pid, signal.SIGKILL)
                        child.wait()
    text = log.read_text(errors='replace')
    rss = re.search(r'Maximum resident set size \(kbytes\): (\d+)', text)
    wall = re.search(r'Elapsed \(wall clock\) time.*: (\S+)', text)
    banner = [l for l in text.splitlines() if l.startswith('Vulkan RT available=')]
    fails = [l for l in text.splitlines() if 'FAIL' in l]
    validation = re.findall(r'validation errors: (\d+)', text)
    print(f'\n[{mode}] exit={child.returncode} rss_kb={rss and rss.group(1)} '
          f'wall={wall and wall.group(1)} validation={validation} banner={banner}')
    for line in fails[:40]:
        print(f'[{mode}] {line}')
    return child.returncode, text, validation


@pytest.mark.parametrize('mode', ['rt-counters'])
def test_mode(mode):
    code, text, validation = run_mode(mode)
    assert code == 0, f'{mode} exited {code}'
    assert validation and all(v == '0' for v in validation), validation
