from pathlib import Path
import json, hashlib, subprocess, sys, time, re
repo=Path('/mnt/d/Shared With Desktop/AI/matter-engine-cpp')
out=repo/'docs/agent/evidence/2026-09-15-direct-source'
out.mkdir(parents=True,exist_ok=True)
prefix=sys.argv[1]
prior=json.loads((repo/'docs/agent/evidence/2026-09-15-material-identity/categorical-integration-source.json').read_text())
paths=list(dict.fromkeys(list(prior['sources'])+['cmake/MatterViewer.cmake','MatterEngine3/shaders_vk/vt_bc_encode.comp','MatterEngine3/shaders_vk/vt_surface_tape.glsl','MatterEngine3/src/render/vt_surface_tape.h','MatterEngine3/src/terrain_field.cpp','MatterEngine3/src/terrain_field.h','MatterEngine3/src/world_base.js.h','MatterEngine3/tests/surface_field_tests.cpp','MatterEngine3/tests/world_definition_tests.cpp','MatterEngine3/tests/eval_world_tests.cpp']))
sha=lambda p:hashlib.sha256((repo/p).read_bytes()).hexdigest()
sources={p:sha(p) for p in paths}
manifest=out/(prefix+'-source.json')
manifest.write_text(json.dumps({'sources':sources},indent=2)+'\n')
checks=[]; failed=False
for item in sys.argv[2:]:
 kind,name=item.split(':',1)
 if kind=='build':cmd=['./tools/build-windows-from-wsl.sh','RelWithDebInfo',name]
 elif kind=='cpu':
  command='Set-Location -LiteralPath "D:/Shared With Desktop/AI/matter-engine-cpp/MatterEngine3/tests"; & "../../MatterEditor/build/cmake/windows-msvc/relwithdebinfo/'+name+'.exe"; exit $LASTEXITCODE'
  cmd=['/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe','-NoProfile','-NonInteractive','-Command',command]
 else:
  exe='vt_compositor_tests' if name=='compositor' else 'vulkan_smoke_tests'
  command='$env:MATTER_VK_SMOKE_MODE="'+name+'"; $env:MATTER_VK_VALIDATION="1"; Set-Location -LiteralPath "D:/Shared With Desktop/AI/matter-engine-cpp/MatterEditor"; & "./build/cmake/windows-msvc/relwithdebinfo/'+exe+'.exe"; exit $LASTEXITCODE'
  cmd=['/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe','-NoProfile','-NonInteractive','-Command',command]
 log=out/(prefix+'-'+kind+'-'+name+'.log');start=time.monotonic()
 with log.open('w') as f:run=subprocess.run(cmd,cwd=repo,stdout=f,stderr=subprocess.STDOUT)
 content=log.read_text(errors='replace')
 row={'kind':kind,'name':name,'command':cmd,'exit':run.returncode,'seconds':time.monotonic()-start,'log':str(log.relative_to(repo)),'source_changes':[p for p,h in sources.items() if sha(p)!=h],'validation_errors':[s for s in content.splitlines() if re.search(r'validation errors: [1-9]|Validation Error|VUID-',s)]}
 checks.append(row);(out/(prefix+'-checks.json')).write_text(json.dumps(checks,indent=2)+'\n');print(json.dumps(row),flush=True)
 failed |= bool(run.returncode or row['validation_errors'])
 if row['source_changes'] or (kind=='build' and run.returncode):break
files=['MatterEditor/build/cmake/windows-msvc/relwithdebinfo/'+n+'.exe' for n in ['vulkan_smoke_tests','vt_compositor_tests','surface_field_tests','world_definition_tests','eval_world_tests']]+['MatterEditor/build/windows-msvc/editor.exe']
manifest.write_text(json.dumps({'sources':sources,'binaries':{p:sha(p) for p in files if (repo/p).exists()}},indent=2)+'\n')
raise SystemExit(1 if failed or any(r['source_changes'] for r in checks) else 0)
