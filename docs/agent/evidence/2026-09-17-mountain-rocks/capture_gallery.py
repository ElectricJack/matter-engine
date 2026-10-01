"""Native rock geometry review, bound to the successful build's source manifest."""
import hashlib,json,re,shutil,subprocess,sys,time
from pathlib import Path

out=Path(__file__).resolve().parent;repo=out.parents[3]
label,prefix=sys.argv[1:3];assert re.fullmatch(r'v[0-9]+',label)
material_controls='material-controls' in sys.argv[3:]
authored_density='authored-density' in sys.argv[3:]
run=out/label;assert not run.exists();run.mkdir()
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
manifest=json.loads((out.parent/'2026-09-16-shared-vt-pixels'/f'{prefix}-build-manifest.json').read_text())
assert not manifest['source_changes'] and all(b['exit']==0 for b in manifest['builds'])
assert all(sha(repo/p)==h for p,h in manifest['sources'].items())
exe=repo/'MatterEditor/build/windows-msvc/editor.exe';binary=sha(exe)
assert binary==manifest['binaries'][str(exe.relative_to(repo))]
capture=Path('/mnt/c/tmp')/f'matter-mountain-rock-gallery-{label}';assert not capture.exists()
win=f'C:/tmp/matter-mountain-rock-gallery-{label}'
views=[('overview','10 9 14 0 .4 0'),('block','1.3 2.4 -.4 -1.7 .6 -3.6'),
       ('slab','1.3 2.4 3.2 -1.7 .35 0'),('weathered','1.3 2.4 6.8 -1.7 .7 3.6')]
lines=['wait_event bake.finished 180','render_path raster','set render.fog.density 0',
 'set render.clouds.layer0_max_density 0','set render.cloud_shadows.enabled false',
 'set render.lighting.sun_elevation_deg 55','set render.lighting.sun_azimuth_deg 30',
 'set render.lighting.sun_tint [1,1,1]','set render.lighting.sun_multiplier 1',
 'set render.lighting.sky_irradiance_multiplier 0.5','set render.lighting.day_ambient_multiplier 0.5']
shots=[]
for name,camera in views:
 lines += [f'cam {camera}','wait_frames 120',f'stats {name}']
 for mode,commands in [('lit',[]),('wireframe',['wireframe on'])]:
  shot=f'{name}-{mode}.png';shots.append(shot)
  lines += commands+['wait_frames 30',f'shot {win}/{shot}']
 lines += ['wireframe off']
 if material_controls:
  for mode,commands in [('albedo',['set viewer.debug.debug_view_mode 4']),
      ('normal',['set viewer.debug.debug_view_mode 1']),
      ('flat',['set viewer.debug.debug_view_mode 0','set render.pom.enabled false'])]:
   shot=f'{name}-{mode}.png';shots.append(shot)
   lines += commands+['wait_frames 60',f'shot {win}/{shot}']
  lines += ['set render.pom.enabled true']
 lines += ['render_path native_rt','wait_frames 90']
 shot=f'{name}-rt-lit.png';shots.append(shot)
 lines += [f'shot {win}/{shot}']
 if material_controls:
  shot=f'{name}-rt-flat.png';shots.append(shot)
  lines += ['set render.pom.enabled false','wait_frames 60',f'shot {win}/{shot}','set render.pom.enabled true']
 lines += ['render_path raster']
lines += ['quit']
timeline=run/'timeline.txt';timeline.write_text('\n'.join(lines)+'\n')
win_timeline='D:/Shared With Desktop/AI/matter-engine-cpp/'+str(timeline.relative_to(repo))
props=repo/'projects/world_demo/scenes/texturing/terrain/RockFormationProof/props.json'
original=props.read_bytes() if props.exists() else None
command=f'''Get-ChildItem Env:MATTER_* | Remove-Item; & py -3 MatterEngine3/tools/drive.py --world RockFormationProof --timeline "{win_timeline}" --out-dir "{win}" --editor "D:/Shared With Desktop/AI/matter-engine-cpp/MatterEditor/build/windows-msvc/editor.exe" --timeout 180 --hide-ui --env MATTER_HIDE_WINDOW=1 --env MATTER_VK_VALIDATION=1 --env MATTER_WINDOW_WIDTH=1280 --env MATTER_WINDOW_HEIGHT=800; exit $LASTEXITCODE'''
if material_controls and not authored_density:
 command=command.replace('; exit $LASTEXITCODE',' --env MATTER_VT_PROP_TEXELS_PER_METER=256; exit $LASTEXITCODE')
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
audit={'editor_exit':process.returncode,'seconds':time.monotonic()-start,'editor_sha256':binary,
 'material_controls':material_controls,'prop_texels_per_m':256 if material_controls and not authored_density else 'asset policy',
 'density_override':material_controls and not authored_density,
 'binary_unchanged':sha(exe)==binary,'source_changes':[p for p,h in manifest['sources'].items() if sha(repo/p)!=h],
 'missing_shots':[p for p in shots if not (run/p).exists() or not (run/(p+'.done')).exists()],
 'validation_errors':[l for l in log.splitlines() if re.search(r'validation(?:\s+|_)error',l,re.I)],
 'command_failures':[l for l in log.splitlines() if re.search(r'unknown prop|set:.*(?:invalid|failed|unknown)|render_path:.*(?:unavailable|expected)|wireframe:.*unavailable',l)],
 'files':{p.name:sha(p) for p in run.iterdir() if p.is_file()}}
audit['passed']=not audit['editor_exit'] and audit['binary_unchanged'] and not any(audit[k] for k in ('source_changes','missing_shots','validation_errors','command_failures'))
if material_controls:
 match=re.search(r'STATSVT,overview,active=1,variants=(\d+)/',log)
 audit['material_owner_check']={'expected_min':13,'observed':int(match.group(1)) if match else 0}
 audit['material_owner_check']['passed']=audit['material_owner_check']['observed']>=13
 audit['passed'] &= audit['material_owner_check']['passed']
(run/'audit.json').write_text(json.dumps(audit,indent=2)+'\n')
print(json.dumps({k:v for k,v in audit.items() if k!='files'},indent=2))
raise SystemExit(0 if audit['passed'] else 1)
