"""Out-and-back native rock LOD motion with GPU draw-authority and VT traces."""
import hashlib,json,math,re,shutil,struct,subprocess,sys,time
from pathlib import Path

out=Path(__file__).resolve().parent;repo=out.parents[3]
label,prefix=sys.argv[1:3];assert re.fullmatch(r'motion-v[0-9]+',label)
dest=out/label;assert not dest.exists();dest.mkdir()
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
build=json.loads((out.parent/'2026-09-16-shared-vt-pixels'/f'{prefix}-build-manifest.json').read_text())
assert not build['source_changes'] and all(r['exit']==0 for r in build['builds'])
assert all(sha(repo/p)==h for p,h in build['sources'].items())
exe=repo/'MatterEditor/build/windows-msvc/editor.exe';binary=sha(exe)
assert binary==build['binaries'][str(exe.relative_to(repo))]
script="""await import('./projects/world_demo/tests/castle_shared_lib_hooks.mjs');
const {mountainRockScaleViews}=await import('./projects/world_demo/shared-lib/mountain_rock_scale_proof.js');
console.log(JSON.stringify(mountainRockScaleViews().find(v=>v.name==='size-32-near')));"""
view=json.loads(subprocess.check_output(['node','--input-type=module','-e',script],cwd=repo,text=True))
target=view['camera'][3:];direction=[view['camera'][0]-target[0],.18,view['camera'][2]-target[2]]
length=math.sqrt(sum(v*v for v in direction));direction=[v/length for v in direction]
poses=[]
for i in range(900):
    phase=i/899;travel=1-abs(2*phase-1)
    distance=32*3*(80/3)**travel
    poses.append([target[k]+distance*direction[k] for k in range(3)]+target)
path=dest/'camera-path.txt';path.write_text('\n'.join(' '.join(f'{v:.9g}' for v in p) for p in poses)+'\n')
win_repo='D:/Shared With Desktop/AI/matter-engine-cpp/'
win_path=win_repo+str(path.relative_to(repo))
props=repo/'projects/world_demo/scenes/texturing/terrain/RockScaleProof/props.json'
original=props.read_bytes() if props.exists() else None
runs=[]
for renderer in ['raster','native_rt']:
    folder=dest/renderer;folder.mkdir()
    native=Path('/mnt/c/tmp')/f'matter-rock-{label}-{renderer}';assert not native.exists()
    win=f'C:/tmp/matter-rock-{label}-{renderer}';tag='rt' if renderer=='native_rt' else 'raster'
    names=[f'move-{tag}-{i:02d}.png' for i in range(21)]
    lines=['wait_event bake.finished 180',f'render_path {renderer}',
      'set render.fog.density 0','set render.clouds.layer0_max_density 0',
      'set render.cloud_shadows.enabled false','set render.lighting.sun_elevation_deg 25',
      'set render.lighting.sun_azimuth_deg -120','set render.lighting.sun_tint [1,1,1]',
      'set render.lighting.sun_multiplier 1','set render.lighting.sky_irradiance_multiplier 0.5',
      'set render.lighting.day_ambient_multiplier 0.5','wait_frames 320']
    for i,name in enumerate(names):lines += [f'stats move-{i:02d}',f'shot {win}/{name}','wait_frames 30']
    # The path exits after all 900 presented poses and its readback drain. All
    # screenshots precede that exit; the final wait leaves the rest in motion.
    lines += ['wait_frames 1000','quit']
    timeline=folder/'timeline.txt';timeline.write_text('\n'.join(lines)+'\n')
    command=f'''Get-ChildItem Env:MATTER_* | Remove-Item; & py -3 MatterEngine3/tools/drive.py --world RockScaleProof --timeline "{win_repo+str(timeline.relative_to(repo))}" --out-dir "{win}" --editor "{win_repo}MatterEditor/build/windows-msvc/editor.exe" --timeout 240 --hide-ui --env MATTER_HIDE_WINDOW=1 --env MATTER_VK_VALIDATION=1 --env MATTER_WINDOW_WIDTH=1280 --env MATTER_WINDOW_HEIGHT=800 --env MATTER_VT_TRACE={win}/vt-trace.jsonl --env MATTER_LOD_TRACE={win}/lod-trace.txt --env "MATTER_CAM_PATH={win_path}" --env MATTER_CAM_PATH_WARMUP=300 --env MATTER_CAM_PATH_EXIT=1; exit $LASTEXITCODE'''
    start=time.monotonic()
    try:
        with (folder/'driver.log').open('w') as log:
            result=subprocess.run(['/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe','-NoProfile','-Command',command],cwd=repo,stdout=log,stderr=subprocess.STDOUT)
    finally:
        if props.exists():
            (folder/'saved-props.json').write_bytes(props.read_bytes())
            if original is None:props.unlink()
            else:props.write_bytes(original)
    shutil.copytree(native,folder,dirs_exist_ok=True)
    log=(folder/'log.txt').read_text(errors='replace');lod=(folder/'lod-trace.txt').read_text()
    events={}
    for token,cluster,before,after,frame in re.findall(r'^E (\d+) (\d+) (\S+) (\S+) #f(\d+)$',lod,re.M):
        events.setdefault(token+':'+cluster,[]).append({'before':before,'after':after,'pose':int(frame)})
    traversed=[key for key,items in events.items() if {'0','1','2'}<={r['after'] for r in items}
               and items[-1]['after']=='0']
    trace=[json.loads(l) for l in (folder/'vt-trace.jsonl').read_text().splitlines() if l.strip()]
    columns=trace[0]['columns'];rows=[dict(zip(columns,r['values'])) for r in trace[1:] if 'values' in r]
    row={'renderer':renderer,'editor_exit':result.returncode,'seconds':time.monotonic()-start,
      'path_complete':'MATTER_CAM_PATH: complete' in log,'lod_events':events,'round_trip_tokens':traversed,
      'lod_errors':[l for l in lod.splitlines() if l.startswith('!')],
      'validation_errors':[l for l in log.splitlines() if re.search(r'validation(?:\s+|_)error',l,re.I)],
      'missing_shots':[n for n in names if not (folder/n).exists() or not (folder/(n+'.done')).exists()],
      'wrong_dimensions':[n for n in names if (folder/n).exists() and struct.unpack('>II',(folder/n).read_bytes()[16:24])!=(1280,800)],
      'source_changes':[p for p,h in build['sources'].items() if sha(repo/p)!=h],
      'binary_unchanged':sha(exe)==binary,
      'vt_peak':{k:max(r[k] for r in rows) for k in ['variants','pool_used','queue','mandatory_queue','oldest_mandatory_age_frames','evictions_total','fills_failed_total','requests_dropped_total']},
      'visual_approval':False}
    row['passed']=row['editor_exit']==0 and row['path_complete'] and bool(traversed) and row['binary_unchanged'] and not any(row[k] for k in ['lod_errors','validation_errors','missing_shots','wrong_dimensions','source_changes']) and row['vt_peak']['fills_failed_total']==0
    (folder/'audit.json').write_text(json.dumps(row,indent=2)+'\n');runs.append(row)
    (dest/'audit.json').write_text(json.dumps({'runs':runs,'passed':len(runs)==2 and all(r['passed'] for r in runs)},indent=2)+'\n')
    print(json.dumps({k:v for k,v in row.items() if k!='lod_events'},indent=2),flush=True)
    if not row['passed']:raise SystemExit(1)
