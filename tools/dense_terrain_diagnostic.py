"""Bounded native GPU experiment using a generated copy of StreamMountain.

Run with native Windows Python from the repo. The fixture retains the exact
field/surface/sector code and 0.25m spacing, limits reach to 32m, and excludes
scatter. Production scenes are not modified. Each run records its source and
environment; the temporary scene is removed afterward. OS disk cache is not
flushed. 'Cold' here means a new fixture identity, not a cold physical disk.
"""
import json, os, re, subprocess, time
from pathlib import Path

repo=Path(__file__).resolve().parent.parent
out=Path(os.environ.get('MATTER_DIAGNOSTIC_OUT','C:/tmp/matter-dense-diagnostic-gpu-v1'))
out.mkdir(exist_ok=False)
scene=repo/'projects/world_demo/scenes/geometry/DenseTerrainDiagnostic'
if scene.exists(): raise RuntimeError('diagnostic scene already exists')
original=repo/'projects/world_demo/scenes/streaming/StreamMountain'
source=(original/'StreamMountain.js').read_text()
source=source.replace('terrainOnly: false','terrainOnly: true')
source=source.replace('__geometryRocks: {material:GEOMETRY_ROCK}', '__geometryRocks: undefined')
source,n=re.subn(r'rings: \[.*?\],','rings: [{ radius: 32.0, rung: 0 }],',source,flags=re.S)
assert n==1
source,n=re.subn(r'terrainBands: \[.*?\],','terrainBands: [{ radius: 32.0, lod: 5 }],',source,flags=re.S)
assert n==1
sector=(original/'objects/WorldSector.js').read_text()
(out/'world.js').write_text(source);(out/'WorldSector.js').write_text(sector)
(scene/'objects').mkdir(parents=True)
(scene/'DenseTerrainDiagnostic.js').write_text(source)
(scene/'objects/WorldSector.js').write_text(sector)

def run(name):
    folder=out/name;folder.mkdir()
    fifo=folder/'commands.txt'
    fifo.write_text('render_path raster\nset render.gi.enabled false\nset render.pom.enabled false\nset render.volumetrics.enabled false\nset render.cloud_shadows.enabled false\n')
    env={k:v for k,v in os.environ.items() if not k.startswith('MATTER_')}
    env.update(json.loads(Path('C:/tmp/matter-blas-mountain/dense-terrain-repair/environment.json').read_text()))
    env.update(MATTER_WORLD='DenseTerrainDiagnostic',MATTER_CMD_FIFO=str(fifo),
        MATTER_CAM='425,35,1456,415,23,1445',MATTER_WINDOW_WIDTH='1280',MATTER_WINDOW_HEIGHT='720',
        MATTER_HIDE_UI='1',MATTER_GEOMETRY_SCENE_ASYNC='1',MATTER_GEOMETRY_PAGES_PROFILE='1',
        MATTER_VT_ENCODED_COOK='1',MATTER_PREPARED_IDENTITY_COOK='1',
        MATTER_STREAM_BAKE_PROFILE='1',MATTER_STREAM_PUBLISH_PROFILE='1',MATTER_STREAM_FILL_PROFILE='1')
    for key in ['MATTER_AGENT_RESULT_FILE','MATTER_SEAM_TRACE']: env.pop(key,None)
    (folder/'environment.json').write_text(json.dumps({k:v for k,v in env.items() if k.startswith('MATTER_')},indent=2))
    def send(command):
        with fifo.open('a') as f:f.write(command+'\n')
    start=time.monotonic();settled=False
    with (folder/'editor.log').open('w') as log:
        p=subprocess.Popen([str(repo/'MatterEditor/build/windows-msvc/editor.exe')],cwd=repo/'MatterEditor',env=env,stdout=log,stderr=subprocess.STDOUT)
        last_count=-1;unchanged=time.monotonic()
        while time.monotonic()-start<300 and p.poll() is None:
            time.sleep(2)
            text=(folder/'editor.log').read_text(errors='replace')
            count=text.count('terrain_prepare_end')
            if count!=last_count:last_count=count;unchanged=time.monotonic()
            queues=re.findall(r'pages assets=\d+ pages=\d+ inflight=(\d+)',text)
            if count>0 and time.monotonic()-unchanged>25 and queues and queues[-1]=='0':
                settled=True;break
        ready_seconds=time.monotonic()-start
        for mode in ['raster','rt','rt_gi','raster_return']:
            send('render_path '+('native_rt' if mode.startswith('rt') else 'raster'))
            send('set render.gi.enabled '+('true' if mode=='rt_gi' else 'false'))
            time.sleep(15)
            for i in range(20):
                if p.poll() is not None:break
                send(f'stats {name}_{mode}_{i}');time.sleep(.5)
            send(f'shot_now {(folder/(mode+".png")).as_posix()}')
        send('quit')
        try:p.wait(timeout=90)
        except subprocess.TimeoutExpired:
            # Stop only the isolated diagnostic process this harness owns.
            p.terminate();p.wait();settled=False
    result=dict(exit_code=p.returncode,settled=settled,ready_seconds=ready_seconds,total_seconds=time.monotonic()-start)
    (folder/'result.json').write_text(json.dumps(result,indent=2));print(name,result,flush=True)

try:
    run('cold');run('warm')
finally:
    (scene/'objects/WorldSector.js').unlink()
    (scene/'DenseTerrainDiagnostic.js').unlink()
    (scene/'objects').rmdir();scene.rmdir()
