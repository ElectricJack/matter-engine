"""Bounded current-revision diagnostic; never changes shader arguments."""
import os
from pathlib import Path
import signal
import subprocess

EXE = Path('/tmp/keen-quest-34/build/vulkan_smoke_tests')
LOG = Path('/tmp/keen-quest-34/diagnostic-rt.log')
TOOL = '/tmp/fleet-rapids-48-epoch18/toolchain/usr'

def test_current_rt_compiler_trace():
    env = dict(os.environ, DISPLAY=':0', VK_ICD_FILENAMES='/usr/share/vulkan/icd.d/lvp_icd.json',
               VK_LAYER_PATH=TOOL + '/share/vulkan/explicit_layer.d',
               LD_LIBRARY_PATH=TOOL + '/lib/x86_64-linux-gnu', LP_NUM_THREADS='1', MESA_GLTHREAD='false',
               MATTER_PIPELINE_PROFILE='1', MATTER_VK_END_FRAME_TRACE='1', MATTER_VK_SMOKE_MODE='rt',
               MATTER_VK_STATIC_RESERVE_CLUSTER_MB='1', MATTER_VK_STATIC_RESERVE_VERTEX_MB='1',
               MATTER_VK_STATIC_RESERVE_INDEX_MB='1', MATTER_VK_CPU_RESERVE_VERTEX_MB='0',
               MATTER_VK_CPU_RESERVE_INDEX_MB='0', MATTER_IMPOSTOR_CELL_PX='16')
    command = ['/usr/bin/time', '-v', 'env',
               'LD_PRELOAD=/tmp/keen-quest-34/trace_llvm.so:/usr/libexec/coreutils/libstdbuf.so',
               '_STDBUF_O=L', '_STDBUF_E=L', str(EXE)]
    with LOG.open('x') as out:
        with subprocess.Popen(command, env=env, cwd=EXE.parent, stdout=out,
                              stderr=subprocess.STDOUT, start_new_session=True) as child:
            try:
                child.wait(timeout=360)
            except subprocess.TimeoutExpired:
                print('Diagnostic reached 360 s bound; inspect compiler enter/exit markers.')
            finally:
                if child.poll() is None:
                    os.killpg(child.pid, signal.SIGTERM)
                    try:
                        child.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        os.killpg(child.pid, signal.SIGKILL)
                        child.wait()
    print(f'diagnostic native exit={child.returncode}; log={LOG}')
    assert child.returncode == 0, 'bounded diagnostic did not finish; see native trace'
