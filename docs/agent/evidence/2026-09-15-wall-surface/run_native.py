from pathlib import Path
import json, hashlib, subprocess, sys, time, re
repo=Path('/mnt/d/Shared With Desktop/AI/matter-engine-cpp')
out=repo/'docs/agent/evidence/2026-09-15-wall-surface'
out.mkdir(parents=True,exist_ok=True)
prefix=sys.argv[1]
prior=json.loads((repo/'docs/agent/evidence/2026-09-15-material-identity/categorical-integration-source.json').read_text())
paths=list(dict.fromkeys(list(prior['sources'])+['cmake/MatterViewer.cmake','MatterEngine3/shaders_vk/vt_bc_encode.comp','MatterEngine3/shaders_vk/vt_surface_tape.glsl','MatterEngine3/src/render/vt_surface_tape.h','MatterEngine3/src/terrain_field.cpp','MatterEngine3/src/terrain_field.h','MatterEngine3/src/world_base.js.h','MatterEngine3/tests/surface_field_tests.cpp','MatterEngine3/tests/world_definition_tests.cpp','MatterEngine3/tests/eval_world_tests.cpp']))
def sha(p):
 with (repo/p).open('rb') as f: return hashlib.file_digest(f,'sha256').hexdigest()
paths=list(dict.fromkeys(paths+['MatterEngine3/src/render/vt_types.h','MatterEngine3/src/render/vt_residency.h','MatterEngine3/src/render/vt_residency.cpp','MatterEngine3/src/render/vt_stub_filler.cpp','MatterEngine3/src/render/vk_context.cpp','MatterEngine3/tests/vt_queue_tests.h']))
paths=list(dict.fromkeys(paths+['MatterEngine3/shaders_vk/vt_parallax.glsl', 'MatterEngine3/shaders_vk/vt_visible_input.glsl', 'MatterEngine3/shaders_vk/rt_primary_inputs.glsl', 'MatterEngine3/shaders_vk/rt_lighting_impl.glsl', 'MatterEngine3/shaders_vk/rt_shadow.rgen', 'MatterEngine3/shaders_vk/vt_chart_resolve.glsl']))
paths=list(dict.fromkeys(paths+['MatterEditor/src/editor_props.cpp']))
paths=list(dict.fromkeys(paths+['libs/MeshChartingLib/include/mesh_charting.h','libs/MeshChartingLib/src/mesh_charting.cpp','libs/MeshChartingLib/tests/mesh_charting_tests.cpp','MatterEditor/Makefile']))
paths=list(dict.fromkeys(paths+['MatterEngine3/tests/solid_face_projection_gpu_tests.cpp','projects/world_demo/objects/texturing/bricks/ClayBrickSource.js','projects/world_demo/shared-lib/clay_brick_source.js','projects/world_demo/shared-lib/clay_brick_material.js','projects/world_demo/scenes/texturing/bricks/ClayBrickGeometryProof/ClayBrickGeometryProof.js']))
paths=list(dict.fromkeys(paths+['MatterEngine3/include/matter/solid_face_projection.h','MatterEngine3/src/render/gpu_meshing/solid_face_projection_common.cpp','MatterEngine3/shaders_vk/solid_face_project.comp']))
paths += [str(p.relative_to(repo)) for p in (repo/'projects/world_demo/scenes/texturing/bricks/ClayBrickMaterialProof').rglob('*.js')]
paths += ['MatterEngine3/src/provider/local_provider.h']
paths += ['MatterEngine3/src/render/gpu_meshing/gpu_solid_face_projector_vk.h','MatterEngine3/src/render/gpu_meshing/gpu_solid_face_projector_vk.cpp','MatterEngine3/tests/solid_face_projection_tests.cpp']
paths += ['MatterEngine3/src/projected_face_cache.h','MatterEngine3/src/projected_face_cache.cpp','MatterEngine3/tests/projected_face_cache_tests.cpp','MatterEngine3/tests/brick_bond_detail_tests.cpp','cmake/manifests/engine-core.sources','cmake/MatterEngine.cmake']
paths += ['MatterEngine3/src/face_material_bake.h','MatterEngine3/src/face_material_bake.cpp','MatterEngine3/src/render/gpu_meshing/gpu_face_material_vk.h','MatterEngine3/src/render/gpu_meshing/gpu_face_material_vk.cpp','MatterEngine3/shaders_vk/face_material.comp','MatterEngine3/tests/face_material_bake_tests.cpp','MatterEngine3/Makefile','cmake/manifests/engine-viewer.sources']
paths += ['MatterEngine3/src/finite_surface_stamp.h', 'MatterEngine3/src/finite_surface_stamp.cpp', 'MatterEngine3/tests/finite_surface_stamp_tests.cpp', 'MatterEngine3/tests/finite_stamp_gpu_probe.h', 'MatterEngine3/shaders_vk/finite_surface_stamp.glsl', 'MatterEngine3/shaders_vk/finite_stamp_probe.comp']
paths += ['MatterEngine3/src/render/vt_finite_sources.h', 'MatterEngine3/src/render/vt_snapshot.h', 'MatterEngine3/src/render/vt_prepare.h', 'MatterEngine3/tests/vt_finite_source_fixture.h', 'MatterEngine3/tests/vt_compositor_tests.cpp', 'MatterEngine3/tests/vt_residency_tests.cpp']
paths += ['MatterEngine3/src/script_host.h', 'MatterEngine3/src/script_host.cpp', 'MatterEngine3/src/dsl_bindings.cpp', 'MatterEngine3/src/part_base.js.h', 'MatterEngine3/src/surface_base.js.inc', 'MatterEngine3/src/finite_surface_base.js.inc', 'MatterEngine3/src/solid_source_js.h', 'MatterEngine3/src/finite_surface_faces.h', 'MatterEngine3/tests/finite_surface_recipe_tests.cpp', 'projects/world_demo/objects/texturing/bricks/ClayBrickSurface.js', 'projects/world_demo/shared-lib/clay_brick_surface.js']
paths += ['MatterEngine3/src/part_surface.h', 'MatterEngine3/src/part_surface.cpp', 'MatterEngine3/src/provider/local_provider.cpp', 'MatterEngine3/src/provider/local_provider.h', 'MatterEngine3/src/matter_engine.cpp', 'MatterEngine3/src/render/vk_scene_renderer.cpp', 'MatterEngine3/src/render/vk_scene_renderer.h', 'MatterEngine3/tests/part_surface_provider_tests.cpp', 'projects/world_demo/objects/texturing/bricks/ClayBrickWallSurface.js', 'projects/world_demo/shared-lib/brick_wall_geometry.js', 'projects/world_demo/scenes/texturing/bricks/ClayBrickWallSurfaceProof/ClayBrickWallSurfaceProof.js']
paths += ['projects/world_demo/shared-lib/clay_brick_wall_surface.js','projects/world_demo/shared-lib/brick_wall_layout.js']
paths += ['projects/world_demo/shared-lib/clay_brick_wall_path.js','projects/world_demo/shared-lib/clay_brick_maze_layout.js','projects/world_demo/objects/texturing/bricks/ClayBrickWallPath.js','projects/world_demo/tests/clay_brick_wall_path_tests.mjs']
paths += [str(p.relative_to(repo)) for p in (repo/'projects/world_demo/scenes/texturing/bricks/ClayBrickMaze').rglob('*.js')]
paths += ['projects/world_demo/shared-lib/wall_weathering.js','projects/world_demo/tests/wall_weathering_tests.mjs']
sources={p:sha(p) for p in paths}
manifest=out/(prefix+'-source.json')
manifest.write_text(json.dumps({'sources':sources},indent=2)+'\n')
checks=[]; failed=False
for item in sys.argv[2:]:
 kind,name=item.split(':',1)
 if kind=='build':cmd=['./tools/build-windows-from-wsl.sh','RelWithDebInfo',name]
 elif kind=='projection':
  command="$env:MATTER_VK_VALIDATION='1'; $env:MATTER_CLAY_FACE_DUMP='D:/tmp/matter-vt/20260915-stamp-vt/faces'; Set-Location -LiteralPath 'D:/Shared With Desktop/AI/matter-engine-cpp'; & './MatterEditor/build/cmake/windows-msvc/relwithdebinfo/solid_face_projection_gpu_tests.exe'; exit $LASTEXITCODE"
  cmd=['/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe','-NoProfile','-NonInteractive','-Command',command]
 elif kind=='rootcpu':
  command='Set-Location -LiteralPath "D:/Shared With Desktop/AI/matter-engine-cpp"; & "./MatterEditor/build/cmake/windows-msvc/relwithdebinfo/'+name+'.exe"; exit $LASTEXITCODE'
  cmd=['/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe','-NoProfile','-NonInteractive','-Command',command]
 elif kind=='cpu':
  command='Set-Location -LiteralPath "D:/Shared With Desktop/AI/matter-engine-cpp/MatterEngine3/tests"; & "../../MatterEditor/build/cmake/windows-msvc/relwithdebinfo/'+name+'.exe"; exit $LASTEXITCODE'
  cmd=['/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe','-NoProfile','-NonInteractive','-Command',command]
 else:
  exe='vt_compositor_tests' if name=='compositor' else 'vulkan_smoke_tests'
  command='$env:MATTER_VK_SMOKE_MODE="'+name+'"; $env:MATTER_VK_VALIDATION="1"; Set-Location -LiteralPath "D:/Shared With Desktop/AI/matter-engine-cpp/MatterEditor"; & "./build/cmake/windows-msvc/relwithdebinfo/'+exe+'.exe"; exit $LASTEXITCODE'
  if kind=='clay': command="$env:MATTER_VT_CLAY_FIXTURE='D:/tmp/matter-vt/20260915-stamp-vt/faces'; "+command
  cmd=['/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe','-NoProfile','-NonInteractive','-Command',command]
 log=out/(prefix+'-'+kind+'-'+name+'.log');start=time.monotonic()
 with log.open('w') as f:run=subprocess.run(cmd,cwd=repo,stdout=f,stderr=subprocess.STDOUT)
 content=log.read_text(errors='replace')
 row={'kind':kind,'name':name,'command':cmd,'exit':run.returncode,'seconds':time.monotonic()-start,'log':str(log.relative_to(repo)),'source_changes':[p for p,h in sources.items() if sha(p)!=h],'validation_errors':[s for s in content.splitlines() if re.search(r'validation errors: [1-9]|Validation Error|VUID-',s)]}
 checks.append(row);(out/(prefix+'-checks.json')).write_text(json.dumps(checks,indent=2)+'\n');print(json.dumps(row),flush=True)
 failed |= bool(run.returncode or row['validation_errors'])
 if row['source_changes'] or failed:break
files=['MatterEditor/build/cmake/windows-msvc/relwithdebinfo/'+n+'.exe' for n in ['part_surface_provider_tests','demand_bake_tests','finite_surface_recipe_tests','solid_source_evaluation_tests','script_host_tests','finite_surface_stamp_tests','face_material_bake_tests','projected_face_cache_tests','brick_bond_detail_tests','solid_face_projection_gpu_tests','solid_face_projection_tests','matter_mesh_charting_tests','vulkan_smoke_tests','vt_compositor_tests','surface_field_tests','world_definition_tests','eval_world_tests']]+['MatterEditor/build/windows-msvc/editor.exe']
manifest.write_text(json.dumps({'sources':sources,'binaries':{p:sha(p) for p in files if (repo/p).exists()}},indent=2)+'\n')
raise SystemExit(1 if failed or any(r['source_changes'] for r in checks) else 0)
