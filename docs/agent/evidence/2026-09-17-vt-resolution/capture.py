"""Settled StreamMountain proxy VT resolution diagnostics with unchanged lit controls."""
import hashlib,json,re,shutil,statistics,struct,subprocess,sys,time
from pathlib import Path
root=Path(__file__).resolve().parent;repo=root.parents[3]
label,prefix=sys.argv[1:3];assert re.fullmatch(r'v[0-9]+',label)
clear_controls=len(sys.argv)>4 and sys.argv[4]=='clear-controls'
rt_controls=clear_controls and len(sys.argv)>5 and sys.argv[5]=='rt-controls'
seam_controls=clear_controls and 'seam-controls' in sys.argv[4:]
run=Path('/mnt/c/tmp')/f'matter-vt-resolution-{label}';assert not run.exists()
manifest=json.loads((root.parent/'2026-09-16-shared-vt-pixels'/f'{prefix}-sources.json').read_text())
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
assert not [p for p,h in manifest.items() if sha(repo/p)!=h]
props=repo/'projects/world_demo/scenes/streaming/StreamMountain/props.json';original=props.read_bytes()
exe=repo/'MatterEditor/build/windows-msvc/editor.exe';binary=sha(exe)
build=json.loads((root.parent/'2026-09-16-shared-vt-pixels'/f'{prefix}-build-manifest.json').read_text())
assert binary==build['binaries'][str(exe.relative_to(repo))]
assert not build['source_changes'] and all(row['exit']==0 for row in build['builds'])
timeline=root/f'{label}-start.txt';timeline.write_text('wait_event bake.finished 900\n')
win_root='D:/Shared With Desktop/AI/matter-engine-cpp/docs/agent/evidence/2026-09-17-vt-resolution'
win_run=f'C:/tmp/matter-vt-resolution-{label}'
command=f'''Get-ChildItem Env:MATTER_* | Remove-Item; & py -3 MatterEngine3/tools/drive.py --world StreamMountain --timeline "{win_root}/{label}-start.txt" --out-dir "{win_run}" --editor "D:/Shared With Desktop/AI/matter-engine-cpp/MatterEditor/build/windows-msvc/editor.exe" --timeout 1000 --hide-ui --env MATTER_HIDE_WINDOW=1 --env MATTER_VK_VALIDATION=1 --env MATTER_WINDOW_WIDTH=1280 --env MATTER_WINDOW_HEIGHT=800 --env MATTER_PROFILE_LOG=1 --env MATTER_BAKE_TRACE={win_run}/bake-trace.json --env MATTER_VT_TRACE={win_run}/vt-trace.jsonl; exit $LASTEXITCODE'''
camera_prefix=sys.argv[3] if len(sys.argv)>3 else prefix
inventory_command='Get-CimInstance Win32_Process -Filter "Name = \'editor.exe\'" | Select-Object ProcessId,ExecutablePath | ConvertTo-Json -Compress'
inventory=subprocess.run(['/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe',
 '-NoProfile','-Command',inventory_command],capture_output=True,text=True,check=True)
other_editors=json.loads(inventory.stdout) if inventory.stdout.strip() else []
if isinstance(other_editors,dict):other_editors=[other_editors]
probe_path=root.parent/'2026-09-16-shared-vt-pixels'/f'{camera_prefix}-test-mountain.log'
probe_text=probe_path.read_text()
cliff_camera=re.search(r'MOUNTAIN_CLIFF_CAMERA ([^\n]+)',probe_text).group(1).strip()
ex,ey,ez,tx,ty,tz=map(float,cliff_camera.split())
# Move six metres along the horizontal tangent for an oblique relief check.
cliff_oblique=f'{ex-(ez-tz):.3f} {ey:.3f} {ez+(ex-tx):.3f} {tx:.3f} {ty:.3f} {tz:.3f}'
views=[('overview','380 90 1600 420 55 1420'),('grazing','380 20.25 1600 400 20.25 1550'),('cliff',cliff_camera),('cliff-grazing',cliff_oblique),('close','380 20.3 1600 380 18.24 1596')]
if len(sys.argv)>6 and sys.argv[6]=='close-cliff-only':views=[v for v in views if v[0] in ('cliff','close')]
commands=[];phase='activate';index=0;cutoff=0;last_state=None;stable=None;probe=0;start=time.monotonic();next_probe=start
names=[f'{view}-{mode}' for view,_ in views for mode in ('lit','albedo','normal','mip','density')]+[f'{view}-flat' for view,_ in views if view!='overview']
if clear_controls:names += [f'{view}-clear-{mode}' for view in ('cliff','close') for mode in ('lit','flat')]
if rt_controls:names += [f'{view}-clear-rt-{mode}' for view in ('cliff','close') for mode in ('lit','flat')]
if seam_controls:names += [f'{view}-seam-{mode}' for view in ('cliff','close') for mode in ('normal-flat','albedo-flat','chart-flat','path','chart','wireframe')]
def send(value):
 commands.append(value)
 with (run/'cmd.txt').open('a') as f:f.write(value+'\n')
 (root/f'{label}-commands.txt').write_text('\n'.join(commands)+'\n')
def move():
 global phase,cutoff,last_state,stable
 send('cam '+views[index][1]+'\nwait_frames 90');phase='settle';cutoff=len(matches);last_state=None;stable=None

def capture():
 view=views[index][0];lines=['set render.pom.horizon_debug 0','set viewer.debug.debug_view_mode 0','wait_frames 90']
 for i in range(30):lines+=['wait_frames 5',f'stats {view}-sample-{i:02d}']
 for mode,debug in [('lit',0),('albedo',4),('normal',1)]:
  lines += [f'set viewer.debug.debug_view_mode {debug}','wait_frames 30',f'shot {win_run}/{view}-{mode}.png']
 for mode,debug in [('mip',10),('density',11)]:
  lines += ['set viewer.debug.debug_view_mode 4',f'set render.pom.horizon_debug {debug}','wait_frames 30',f'shot {win_run}/{view}-{mode}.png']
 lines += ['set render.pom.horizon_debug 0','set viewer.debug.debug_view_mode 0']
 if view.startswith('cliff') or view=='grazing':lines += ['set render.pom.enabled false','wait_frames 90',f'shot {win_run}/{view}-flat.png','set render.pom.enabled true']
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
  if phase=='shots' and (run/f'{views[index][0]}-density.png.done').is_file():
   index+=1
   if index<len(views):move()
   else:
    lines=['set render.pom.enabled false','wait_frames 90',f'shot {win_run}/close-flat.png','set render.pom.enabled true']
    if clear_controls:
     # Additional appearance controls, after all comparable timing/material
     # captures. Preserve the standard views and restore authored props on exit.
     lines += ['set render.clouds.layer0_max_density 0','set render.cloud_shadows.enabled false',
       'set render.fog.density 0','set render.lighting.sky_irradiance_multiplier 0.15',
       'set render.lighting.day_ambient_multiplier 0.15','set render.lighting.sun_multiplier 2']
     for view,camera in [('cliff',cliff_camera),('close',views[-1][1])]:
      lines += [f'cam {camera}','wait_frames 180',f'shot {win_run}/{view}-clear-lit.png',
       'set render.pom.enabled false','wait_frames 90',f'shot {win_run}/{view}-clear-flat.png',
       'set render.pom.enabled true']
      if seam_controls:
       lines += ['set render.pom.enabled false','set viewer.debug.debug_view_mode 1',
         'wait_frames 30',f'shot {win_run}/{view}-seam-normal-flat.png',
         'set viewer.debug.debug_view_mode 4','wait_frames 30',f'shot {win_run}/{view}-seam-albedo-flat.png',
         'set render.pom.horizon_debug 8','wait_frames 30',f'shot {win_run}/{view}-seam-chart-flat.png',
         'set render.pom.enabled true','set render.pom.horizon_debug 9','wait_frames 30',f'shot {win_run}/{view}-seam-path.png',
         'set render.pom.horizon_debug 8','wait_frames 30',f'shot {win_run}/{view}-seam-chart.png',
         'set render.pom.horizon_debug 0','set viewer.debug.debug_view_mode 0','wireframe on',
         'wait_frames 30',f'shot {win_run}/{view}-seam-wireframe.png','wireframe off']
      if rt_controls:
       lines += ['render_path native_rt','wait_frames 90',f'shot {win_run}/{view}-clear-rt-lit.png',
         'set render.pom.enabled false','wait_frames 60',f'shot {win_run}/{view}-clear-rt-flat.png',
         'set render.pom.enabled true','render_path raster']
    send('\n'.join(lines+['quit']));phase='shutdown'
  if now-start>900 and phase!='shutdown':send('quit');phase='shutdown'
  time.sleep(1)
 code=process.wait()
(root/f'{label}-saved-props.json').write_bytes(props.read_bytes());props.write_bytes(original)
dest=root/label;assert not dest.exists();shutil.copytree(run,dest)
text=(dest/'log.txt').read_text(errors='replace')
audit={'camera_probe_log':str(probe_path.relative_to(repo)),'camera_probe_sha256':sha(probe_path),'editor_exit':code,'seconds':time.monotonic()-start,'editor_sha256':binary,'binary_unchanged':sha(exe)==binary,
 'source_changes':[p for p,h in manifest.items() if sha(repo/p)!=h],
 'missing_shots':[n for n in names if not (dest/(n+'.png')).is_file() or not (dest/(n+'.png.done')).is_file()],
 'wrong_dimensions':[n for n in names if (dest/(n+'.png')).is_file() and struct.unpack('>II',(dest/(n+'.png')).read_bytes()[16:24])!=(1280,800)],
 'validation_errors':[l for l in text.splitlines() if re.search(r'validation(?:\s+|_)error',l,re.I)],
 'command_failures':[l for l in text.splitlines() if re.search(r'unknown prop|set:.*(?:invalid|failed|unknown)|render_path:.*(?:unavailable|expected)|wireframe:.*unavailable',l)],
 'render_path_receipts':re.findall(r'^render_path: (raster|native_rt)$',text,re.M),
 'other_editors_before_capture':other_editors,
 'isolation':('Other editors were open before capture; development measurements, not isolated acceptance.'
   if other_editors else 'No other editor process before capture. Other GPU workloads are not controlled; development measurement.'),
 'stats':[l for l in text.splitlines() if l.startswith(('STATS,','STATSVT,'))],
 'files':{p.name:sha(p) for p in dest.iterdir() if p.is_file()},'timings':{}}
for view,_ in views:
 values=[float(l.split(',')[-1]) for l in text.splitlines() if l.startswith(f'STATS,{view}-sample-')]
 audit['timings'][view]={'samples':len(values),'gbuffer_median_ms':statistics.median(values) if values else None}
(dest/'audit.json').write_text(json.dumps(audit,indent=2)+'\n')
print(json.dumps({k:v for k,v in audit.items() if k not in ('files','stats')},indent=2),flush=True)
raise SystemExit(0 if code==0 and audit['binary_unchanged'] and not any(audit[k] for k in ('source_changes','missing_shots','wrong_dimensions','validation_errors','command_failures')) else 1)
