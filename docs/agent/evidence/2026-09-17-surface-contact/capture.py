"""Native separate-receiver proof; freeze input sources and restore scene props."""
import hashlib,json,re,shutil,subprocess,sys,time
from pathlib import Path
root=Path(__file__).resolve().parent;repo=root.parents[3]
label,prefix=sys.argv[1:3];assert re.fullmatch(r'v[0-9]+',label)
eager='--eager' in sys.argv[3:]
extra_env=' --env MATTER_VT_EAGER=1' if eager else ''
run=Path('/mnt/c/tmp')/f'matter-surface-contact-{label}';assert not run.exists()
sources=json.loads((root.parent/'2026-09-16-shared-vt-pixels'/f'{prefix}-sources.json').read_text())
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
assert not [p for p,h in sources.items() if sha(repo/p)!=h]
props=repo/'projects/world_demo/scenes/texturing/terrain/SurfaceContactProof/props.json';original=props.read_bytes()
exe=repo/'MatterEditor/build/windows-msvc/editor.exe';binary=sha(exe)
win_root='D:/Shared With Desktop/AI/matter-engine-cpp/docs/agent/evidence/2026-09-17-surface-contact'
win_run=f'C:/tmp/matter-surface-contact-{label}'
start_file=root/f'{label}-start.txt';start_file.write_text('wait_event bake.finished 300\n')
command=f'''Get-ChildItem Env:MATTER_* | Remove-Item; & py -3 MatterEngine3/tools/drive.py --world SurfaceContactProof --timeline "{win_root}/{label}-start.txt" --out-dir "{win_run}" --editor "D:/Shared With Desktop/AI/matter-engine-cpp/MatterEditor/build/windows-msvc/editor.exe" --timeout 600 --hide-ui --env MATTER_HIDE_WINDOW=1 --env MATTER_VK_VALIDATION=1 --env MATTER_WINDOW_WIDTH=1280 --env MATTER_WINDOW_HEIGHT=800 --env MATTER_PROFILE_LOG=1 --env MATTER_AGENT_RESULT_FILE={win_run}/results.jsonl --env MATTER_VT_PROP_TEXELS_PER_METER=128 --env MATTER_BAKE_TRACE={win_run}/bake-trace.json{extra_env}; exit $LASTEXITCODE'''
views=[('overview','12.2 3.6 14.4 8 .6 8.4'),('close','8.8 1.1 10.5 7.4 .5 8.4'),
 ('grazing','10.7 .65 9.1 6.7 .46 8.5'),('back','12 2 6.8 8 1 7.8'),
 ('depth','12 2.8 7.0 8 .55 6.2'),('shelf','11.4 1.5 10.3 9.5 .72 8.7')]
commands=[];names=[];phase='activate';index=0;cutoff=0;last=None;stable=None;probe=0
start=time.monotonic();next_probe=start

def send(value):
 commands.append(value)
 with (run/'cmd.txt').open('a') as f:f.write(value+'\n')
 (root/f'{label}-commands.txt').write_text('\n'.join(commands)+'\n')

def move():
 global phase,cutoff,last,stable
 send('cam '+views[index][1]+'\nwait_frames 90');phase='settle';cutoff=len(matches);last=None;stable=None

def capture():
 name=views[index][0];lines=['set viewer.debug.debug_view_mode 0','wait_frames 60',f'stats {name}']
 modes=[('lit',0),('albedo',4),('normal',1)] if name in ('overview','close','grazing') else [('lit',0),('albedo',4)]
 def shot(suffix):
  name=views[index][0]+'-'+suffix;names.append(name);lines.append(f'shot {win_run}/{name}.png')
 for mode,debug in modes:
  lines += [f'set viewer.debug.debug_view_mode {debug}','wait_frames 30'];shot(mode)
 lines += ['set viewer.debug.debug_view_mode 0']
 if name in ('close','grazing'):
  lines += ['set render.pom.enabled false','wait_frames 60'];shot('flat');lines += ['set render.pom.enabled true']
 if name=='close':
  lines += ['render_path native_rt','wait_frames 120'];shot('rt');lines += ['render_path raster']
 lines += ['wait_frames 30'];shot('finished')
 send('\n'.join(lines))

with (root/f'{label}-driver.log').open('w') as log:
 process=subprocess.Popen(['/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe','-NoProfile','-Command',command],cwd=repo,stdout=log,stderr=subprocess.STDOUT)
 try:
  while process.poll() is None:
   now=time.monotonic()
   if phase in ('activate','settle') and now>=next_probe and (run/'cmd.txt').exists():
    probe+=1;next_probe=now+5;send(f'stats ready-{probe}')
   text=(run/'log.txt').read_text(errors='replace') if (run/'log.txt').exists() else ''
   matches=re.findall(r'STATSVT,ready-\d+,active=(\d+),variants=(\d+)/[^\n]*?queue=(\d+),',text)
   if phase=='activate' and matches and int(matches[-1][0]):
    send('''render_path raster
set render.lighting.sun_elevation_deg 55
set render.lighting.sun_azimuth_deg 20
set render.lighting.sun_tint [1,1,1]
set render.lighting.sky_tint [1,1,1]
set render.lighting.sun_multiplier 1
set render.lighting.exposure_ev 0
set render.lighting.day_ambient_multiplier 0.5
set render.lighting.sky_irradiance_multiplier 0.7
set render.pom.enabled true''')
    send('agent '+json.dumps({'version':1,'request_id':'receivers','command':'scene.list_objects','args':{'kinds':['baked_root']}}))
    send('agent '+json.dumps({'version':1,'request_id':'snapshot','command':'scene.capture_snapshot','args':{}}))
    move()
   if phase=='settle' and len(matches)>cutoff:
    active,count,queue=map(int,matches[-1]);state=(len(matches),count)
    if state!=last:
     if active and count>=4 and queue==0 and last and last[1]==count:
      if stable is None:stable=now
     else:stable=None
     last=state
    if stable is not None and now-stable>=10:
     print(f'{label} {views[index][0]} settled {now-start:.1f}s variants={count}',flush=True)
     capture();phase='shots'
   if phase=='shots' and (run/f'{views[index][0]}-finished.png.done').is_file():
    index+=1
    if index<len(views):move()
    else:send('quit');phase='shutdown'
   if now-start>500 and phase!='shutdown':send('quit');phase='shutdown'
   time.sleep(1)
  code=process.wait()
 finally:
  # Wait for this session to exit before restoring its saved props.
  if process.poll() is None:
   send('quit');process.wait(timeout=30)
  (root/f'{label}-saved-props.json').write_bytes(props.read_bytes());props.write_bytes(original)
dest=root/label;assert not dest.exists();shutil.copytree(run,dest)
text=(dest/'log.txt').read_text(errors='replace')
audit={'editor_exit':code,'seconds':time.monotonic()-start,'editor_sha256':binary,'binary_unchanged':sha(exe)==binary,
 'source_changes':[p for p,h in sources.items() if sha(repo/p)!=h],
 'eager':eager,'expected_shots':names,'completed_views':index,
 'missing_shots':[n for n in names if not (dest/(n+'.png')).is_file() or not (dest/(n+'.png.done')).is_file()],
 'validation_errors':[l for l in text.splitlines() if re.search(r'validation(?:\s+|_)error',l,re.I)],
 'command_failures':[l for l in text.splitlines() if re.search(r'unknown prop|set:.*(?:invalid|failed|unknown)|render_path:.*unavailable',l)],
 'stats':[l for l in text.splitlines() if l.startswith(('STATS,','STATSVT,'))],
 'files':{p.name:sha(p) for p in dest.iterdir() if p.is_file()},
 'isolation':'Frozen r2 asset editor separately open. Development evidence, not isolated performance acceptance.'}
(dest/'audit.json').write_text(json.dumps(audit,indent=2)+'\n')
print(json.dumps({k:v for k,v in audit.items() if k not in ('stats','files')},indent=2),flush=True)
raise SystemExit(0 if code==0 and index==len(views) and audit['binary_unchanged'] and not any(audit[k] for k in ('source_changes','missing_shots','validation_errors','command_failures')) else 1)
