"""Equal-distance native material review of physical rock size classes."""
import hashlib,json,re,shutil,struct,subprocess,sys,time
from pathlib import Path

out=Path(__file__).resolve().parent;repo=out.parents[3]
label,prefix=sys.argv[1:3]
assert re.fullmatch(r'(?:scale|sunlit)-v[0-9]+',label)
sunlit=label.startswith('sunlit-')
run=out/label;assert not run.exists();run.mkdir()
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
build=json.loads((out.parent/'2026-09-16-shared-vt-pixels'/f'{prefix}-build-manifest.json').read_text())
assert not build['source_changes'] and all(row['exit']==0 for row in build['builds'])
assert all(sha(repo/p)==h for p,h in build['sources'].items())
exe=repo/'MatterEditor/build/windows-msvc/editor.exe';binary=sha(exe)
assert binary==build['binaries'][str(exe.relative_to(repo))]
script="""await import('./projects/world_demo/tests/castle_shared_lib_hooks.mjs');
const {mountainRockScaleViews}=await import('./projects/world_demo/shared-lib/mountain_rock_scale_proof.js');
console.log(JSON.stringify(mountainRockScaleViews()));"""
views=json.loads(subprocess.check_output(['node','--input-type=module','-e',script],cwd=repo,text=True))
if sunlit: views=[v for v in views if v['name']!='overview']
(run/'views.json').write_text(json.dumps(views,indent=2)+'\n')
capture=Path('/mnt/c/tmp')/f'matter-rock-{label}';assert not capture.exists()
win=f'C:/tmp/matter-rock-{label}'
lines=['wait_event bake.finished 180','render_path raster','set render.fog.density 0',
 'set render.clouds.layer0_max_density 0','set render.cloud_shadows.enabled false',
 'set render.lighting.sun_elevation_deg 55','set render.lighting.sun_azimuth_deg 30',
 'set render.lighting.sun_tint [1,1,1]','set render.lighting.sun_multiplier 1',
 'set render.lighting.sky_irradiance_multiplier 0.5','set render.lighting.day_ambient_multiplier 0.5']
shots=[]
if sunlit:
 lines+=['set render.lighting.sun_elevation_deg 25','set render.lighting.sun_azimuth_deg -120']
for view in views:
 name=view['name'];camera=' '.join(f'{v:.9g}' for v in view['camera'])
 lines += [f'cam {camera}','wait_frames 120',f'stats {name}']
 controls=[('lit',[]),('albedo',['set viewer.debug.debug_view_mode 4']),
  ('normal',['set viewer.debug.debug_view_mode 1']),
  ('density',['set viewer.debug.debug_view_mode 4','set render.pom.horizon_debug 11']),
  ('flat',['set render.pom.horizon_debug 0','set viewer.debug.debug_view_mode 0','set render.pom.enabled false'])]
 if sunlit: controls=[entry for entry in controls if entry[0] in ('lit','flat')]
 for mode,commands in controls:
  shot=f'{name}-{mode}.png';shots.append(shot)
  lines+=commands+['wait_frames 60',f'shot {win}/{shot}']
 lines += ['set render.pom.enabled true','render_path native_rt','wait_frames 90']
 for mode,commands in [('rt-lit',[]),('rt-flat',['set render.pom.enabled false'])]:
  shot=f'{name}-{mode}.png';shots.append(shot)
  lines+=commands+['wait_frames 60',f'shot {win}/{shot}']
 lines+=['set render.pom.enabled true','render_path raster']
lines+=['quit']
timeline=run/'timeline.txt';timeline.write_text('\n'.join(lines)+'\n')
win_timeline='D:/Shared With Desktop/AI/matter-engine-cpp/'+str(timeline.relative_to(repo))
props=repo/'projects/world_demo/scenes/texturing/terrain/RockScaleProof/props.json'
original=props.read_bytes() if props.exists() else None
command=f'''Get-ChildItem Env:MATTER_* | Remove-Item; & py -3 MatterEngine3/tools/drive.py --world RockScaleProof --timeline "{win_timeline}" --out-dir "{win}" --editor "D:/Shared With Desktop/AI/matter-engine-cpp/MatterEditor/build/windows-msvc/editor.exe" --timeout 360 --hide-ui --env MATTER_HIDE_WINDOW=1 --env MATTER_VK_VALIDATION=1 --env MATTER_WINDOW_WIDTH=1280 --env MATTER_WINDOW_HEIGHT=800 --env MATTER_VT_TRACE={win}/vt-trace.jsonl; exit $LASTEXITCODE'''
start=time.monotonic()
try:
 with (run/'driver.log').open('w') as log:
  process=subprocess.run(['/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe','-NoProfile','-Command',command],cwd=repo,stdout=log,stderr=subprocess.STDOUT)
finally:
 if props.exists():
  (run/'saved-props.json').write_bytes(props.read_bytes())
  if original is None:props.unlink()
  else:props.write_bytes(original)
shutil.copytree(capture,run,dirs_exist_ok=True)
log=(run/'log.txt').read_text(errors='replace')
sources=set(re.findall(r'\[part-surface\] part=([0-9a-f]+) direct recipe_bytes=\d+ source_images=0',log))
audit={'editor_exit':process.returncode,'seconds':time.monotonic()-start,'editor_sha256':binary,
 'sunlit_controls':sunlit,
 'density_override':False,'binary_unchanged':sha(exe)==binary,
 'source_changes':[p for p,h in build['sources'].items() if sha(repo/p)!=h],
 'missing_shots':[p for p in shots if not (run/p).exists() or not (run/(p+'.done')).exists()],
 'wrong_dimensions':[p for p in shots if (run/p).exists() and struct.unpack('>II',(run/p).read_bytes()[16:24])!=(1280,800)],
 'sun_control_matches':not sunlit or 'set: render.lighting.sun_azimuth_deg = -120\n' in log,
 'direct_source_count':len(sources),
 'validation_errors':[l for l in log.splitlines() if re.search(r'validation(?:\s+|_)error',l,re.I)],
 'command_failures':[l for l in log.splitlines() if re.search(r'unknown prop|set:.*(?:invalid|failed|unknown)|render_path:.*(?:unavailable|expected)',l)],
 'files':{p.name:sha(p) for p in run.iterdir() if p.is_file()}}
audit['passed']=not audit['editor_exit'] and audit['binary_unchanged'] and len(sources)==4 and audit['sun_control_matches'] and not any(audit[k] for k in ('source_changes','missing_shots','wrong_dimensions','validation_errors','command_failures'))
(run/'audit.json').write_text(json.dumps(audit,indent=2)+'\n')
print(json.dumps({k:v for k,v in audit.items() if k!='files'},indent=2))
raise SystemExit(0 if audit['passed'] else 1)
