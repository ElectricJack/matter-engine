import os
from pathlib import Path
import subprocess

OUT=Path('/tmp/eager-nexus-71-epoch4')
EXE=OUT/'gpu-build/vulkan_smoke_tests'

def environment():
    env=os.environ.copy()
    env.update(DISPLAY=':0', VK_ICD_FILENAMES='/usr/share/vulkan/icd.d/lvp_icd.json',
        VK_LAYER_PATH='/tmp/fleet-rapids-48-epoch18/toolchain/usr/share/vulkan/explicit_layer.d',
        LD_LIBRARY_PATH='/tmp/fleet-rapids-48-epoch18/toolchain/usr/lib/x86_64-linux-gnu',
        LP_NUM_THREADS='1', MESA_GLTHREAD='false',
        MATTER_PIPELINE_PROFILE='1', MATTER_VK_STATIC_RESERVE_CLUSTER_MB='1',MATTER_VK_STATIC_RESERVE_VERTEX_MB='1',
        MATTER_VK_STATIC_RESERVE_INDEX_MB='1',MATTER_VK_CPU_RESERVE_VERTEX_MB='0',
        MATTER_VK_CPU_RESERVE_INDEX_MB='0',MATTER_IMPOSTOR_CELL_PX='16')
    return env

def test_display():
    r=subprocess.run(['xdpyinfo'],env=environment(),capture_output=True,text=True,timeout=10)
    (OUT/'display.log').write_text(r.stdout+r.stderr)
    assert r.returncode==0,r.stderr
    print('\n'.join(r.stdout.splitlines()[:10]))

def run(mode,timeout):
    env=environment();env['MATTER_VK_SMOKE_MODE']=mode
    with (OUT/('gpu-'+mode+'.log')).open('w') as log:
        r=subprocess.run(['stdbuf','-oL','-eL',str(EXE)],env=env,stdout=log,stderr=subprocess.STDOUT,timeout=timeout)
    output=(OUT/('gpu-'+mode+'.log')).read_text()
    print(output)
    assert r.returncode==0
    assert 'validation errors: 0' in output
    return output

def test_gpu_quick():
    run('default',60)

def test_gpu_sparse_admission():
    output=run('sparse-admission',900)
    assert 'SPARSE_ADMISSION_GPU mixed_ordinary_lod_primary_shadow_rollback_and_retry=pass' in output

def test_editor_first_load_capture():
    root=Path('/home/jkern/dev/matter-engine-cpp/.aq/worktrees/slot-3')
    shot=OUT/'floor-demo.png'
    shot.unlink(missing_ok=True)
    env=environment();env.update(MATTER_WORLD='FloorDemo',MATTER_SCREENSHOT=str(shot),MATTER_SCREENSHOT_SETTLE='3')
    with (OUT/'editor-first-load.log').open('w') as log:
        r=subprocess.run(['stdbuf','-oL','-eL',str(root/'MatterEditor/build/linux/editor')],cwd=root/'MatterEditor',env=env,stdout=log,stderr=subprocess.STDOUT,timeout=600)
    text=(OUT/'editor-first-load.log').read_text()
    print(text[-16000:])
    assert r.returncode==0
    assert shot.exists() and shot.stat().st_size>0
    assert 'bake ready' in text
    assert 'Scene admission rejected' not in text

def test_editor_raster_first_load_capture():
    root=Path('/home/jkern/dev/matter-engine-cpp/.aq/worktrees/slot-3')
    shot=OUT/'floor-demo-raster.png'
    shot.unlink(missing_ok=True)
    env=environment();env.update(MATTER_WORLD='FloorDemo',MATTER_SCREENSHOT=str(shot),
        MATTER_SCREENSHOT_SETTLE='3',MATTER_DISABLE_VK_RT='1',MATTER_WINDOW_WIDTH='320',
        MATTER_WINDOW_HEIGHT='240',MATTER_HIDE_UI='1')
    with (OUT/'editor-raster-first-load.log').open('w') as log:
        r=subprocess.run(['stdbuf','-oL','-eL',str(root/'MatterEditor/build/linux/editor')],cwd=root/'MatterEditor',env=env,stdout=log,stderr=subprocess.STDOUT,timeout=600)
    text=(OUT/'editor-raster-first-load.log').read_text()
    print(text[-16000:])
    assert r.returncode==0
    assert shot.exists() and shot.stat().st_size>0
    assert 'bake ready' in text
    assert 'Scene admission rejected' not in text
    assert 'enabled=false' in text
