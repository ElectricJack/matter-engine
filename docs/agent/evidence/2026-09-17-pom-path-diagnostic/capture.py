"""Settled StreamMountain POM route diagnostic, with normal output controls."""
import hashlib,json,re,shutil,statistics,subprocess,sys,time
from pathlib import Path
root=Path(__file__).resolve().parent;repo=root.parents[3]
label,prefix=sys.argv[1:3];assert re.fullmatch(r'v[0-9]+',label)
run=Path('/mnt/c/tmp')/f'matter-pom-path-diagnostic-{label}';assert not run.exists()
manifest=json.loads((root.parent/'2026-09-16-shared-vt-pixels'/f'{prefix}-sources.json').read_text())
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
assert not [p for p,h in manifest.items() if sha(repo/p)!=h]
props=repo/'projects/world_demo/scenes/streaming/StreamMountain/props.json';original=props.read_bytes()
exe=repo/'MatterEditor/build/windows-msvc/editor.exe';binary=sha(exe)
timeline=root/f'{label}-start.txt';timeline.write_text('wait_event bake.finished 900\n')
win_root='D:/Shared With Desktop/AI/matter-engine-cpp/docs/agent/evidence/2026-09-17-pom-path-diagnostic'
win_run=f'C:/tmp/matter-pom-path-diagnostic-{label}'
command=f'''Get-ChildItem Env:MATTER_* | Remove-Item; & py -3 MatterEngine3/tools/drive.py --world StreamMountain --timeline "{win_root}/{label}-start.txt" --out-dir "{win_run}" --editor "D:/Shared With Desktop/AI/matter-engine-cpp/MatterEditor/build/windows-msvc/editor.exe" --timeout 1000 --hide-ui --env MATTER_HIDE_WINDOW=1 --env MATTER_VK_VALIDATION=1 --env MATTER_WINDOW_WIDTH=1280 --env MATTER_WINDOW_HEIGHT=800 --env MATTER_PROFILE_LOG=1 --env MATTER_BAKE_TRACE={win_run}/bake-trace.json; exit $LASTEXITCODE'''
views=[('overview','380 90 1600 420 55 1420'),('grazing','380 20.25 1600 400 20.25 1550'),('close','380 20.3 1600 380 18.24 1596')]
commands=[];phase='activate';index=0;cutoff=0;last_state=None;stable=None;probe=0;start=time.monotonic();next_probe=start
names=[f'{view}-{mode}' for view,_ in views for mode in ('lit','albedo','normal','path')]
def send(value):
 commands.append(value)
 with (run/'cmd.txt').open('a') as f:f.write(value+'\n')
 (root/f'{label}-commands.txt').write_text('\n'.join(commands)+'\n')
def move():
 global phase,cutoff,last_state,stable
 send('cam '+views[index][1]+'\nwait_frames 90');phase='settle';cutoff=len(matches);last_state=None;stable=None

def capture():
 view=views[index][0]
 lines=['set render.pom.horizon_debug 0','set viewer.debug.debug_view_mode 0','wait_frames 120']
 for i in range(30):lines+=['wait_frames 5',f'stats {view}-on-{i:02d}']
 lines += [f'shot {win_run}/{view}-lit.png']
 for mode,debug in [('albedo',4),('normal',1)]:
  lines += [f'set viewer.debug.debug_view_mode {debug}','wait_frames 30',f'shot {win_run}/{view}-{mode}.png']
 lines += ['set render.pom.horizon_debug 9','set viewer.debug.debug_view_mode 4','wait_frames 90',
           f'shot {win_run}/{view}-path.png','set render.pom.horizon_debug 0','set viewer.debug.debug_view_mode 0']
 send('\n'.join(lines))

with (root/f'{label}-driver.log').open('w') as log:
 process=subprocess.Popen(['/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe','-NoProfile','-Command',command],cwd=repo,stdout=log,stderr=subprocess.STDOUT)
 while process.poll() is None:
  now=time.monotonic()
  if phase in ('activate','settle') and now>=next_probe and (run/'cmd.txt').exists():
   probe+=1;next_probe=now+5;send(f'stats ready-{probe}')
  text=(run/'log.txt').read_text(errors='replace') if (run/'log.txt').exists() else ''
  matches=re.findall(r'STATSVT,ready-\d+,active=(\d+),variants=(\d+)/[^\n]*?queue=(\d+),',text)
  if phase=='activate' and matches and int(matches[-1][0]):
   send('''render_path raster
set render.lighting.sun_elevation_deg 70
set render.lighting.sun_azimuth_deg 0
set render.lighting.sun_tint [1,1,1]
set render.lighting.sky_tint [1,1,1]
set render.lighting.sun_multiplier 1
set render.lighting.exposure_ev 0
set render.lighting.day_ambient_multiplier 0.5
set render.lighting.sky_irradiance_multiplier 0.7
set render.pom.enabled true''')
   move()
  if phase=='settle' and len(matches)>cutoff:
   active,count,queue=map(int,matches[-1]);state=(len(matches),count)
   if state!=last_state:
    if active and count>=789 and queue==0 and last_state and last_state[1]==count:
     if stable is None:stable=now
    else:stable=None
    last_state=state
   if stable is not None and now-stable>=15:
    print(f'{label} {views[index][0]} settled {now-start:.1f}s variants={count}',flush=True)
    capture();phase='shots'
  if phase=='shots' and (run/f'{views[index][0]}-path.png.done').is_file():
   index+=1
   if index<len(views):move()
   else:send('quit');phase='shutdown'
  if now-start>900 and phase!='shutdown':send('quit');phase='shutdown'
  time.sleep(1)
 code=process.wait()
(root/f'{label}-saved-props.json').write_bytes(props.read_bytes());props.write_bytes(original)
dest=root/label;assert not dest.exists();shutil.copytree(run,dest)
text=(dest/'log.txt').read_text(errors='replace')
audit={'editor_exit':code,'seconds':time.monotonic()-start,'editor_sha256':binary,'binary_unchanged':sha(exe)==binary,
 'source_changes':[p for p,h in manifest.items() if sha(repo/p)!=h],
 'missing_shots':[n for n in names if not (dest/(n+'.png')).is_file() or not (dest/(n+'.png.done')).is_file()],
 'validation_errors':[l for l in text.splitlines() if 'Validation Error' in l],
 'command_failures':[l for l in text.splitlines() if re.search(r'unknown prop|set:.*(?:invalid|failed|unknown)|render_path:.*unavailable',l)],
 'isolation':'Frozen r2 editor PID 9056 independently open; development measurements, not isolated acceptance.',
 'stats':[l for l in text.splitlines() if l.startswith(('STATS,','STATSVT,'))],
 'files':{p.name:sha(p) for p in dest.iterdir() if p.is_file()},'timings':{}}
for view,_ in views:
 for phase in ('on',):
  values=[float(l.split(',')[-1]) for l in text.splitlines() if l.startswith(f'STATS,{view}-{phase}-')]
  audit['timings'][f'{view}-{phase}']={'samples':len(values),'gbuffer_median_ms':statistics.median(values) if values else None}
(dest/'audit.json').write_text(json.dumps(audit,indent=2)+'\n')
print(json.dumps({k:v for k,v in audit.items() if k not in ('files','stats')},indent=2),flush=True)
raise SystemExit(0 if code==0 and audit['binary_unchanged'] and not any(audit[k] for k in ('source_changes','missing_shots','validation_errors','command_failures')) else 1)
