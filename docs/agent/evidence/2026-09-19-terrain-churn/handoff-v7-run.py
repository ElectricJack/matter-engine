import os,json,pathlib,subprocess,math
root=pathlib.Path('D:/Shared With Desktop/AI/matter-engine-cpp')
out=pathlib.Path('C:/tmp/matter-blas-mountain/stationary-churn-handoff-v7')
env=os.environ.copy()
for k in list(env):
 if k.startswith('MATTER_'):env.pop(k)
env.update(json.loads(pathlib.Path('C:/tmp/matter-blas-mountain/sector-frustum-v1/environment.json').read_text()))
env.update(MATTER_VK_CPU_RESERVE_VERTEX_MB='2048',MATTER_VK_CPU_RESERVE_INDEX_MB='256',MATTER_VK_STATIC_RESERVE_VERTEX_MB='2048')
env['MATTER_CMD_FIFO']=str(out/'commands.txt')
env['MATTER_PROFILE_TRACE']=str(out/'profile.json')
env['MATTER_VT_TRACE']=str(out/'vt.jsonl')
env['MATTER_VOLUMETRIC_SECTORS']='1'
env['MATTER_GEOMETRY_VISIBLE_PROFILE']='1'
env['MATTER_PREPARED_SECTOR_CACHE_ONLY']='0'
env['MATTER_GEOMETRY_CACHE_ONLY']='0'
env['MATTER_GEOMETRY_SCENE_ASYNC']='0'
env['MATTER_HIDE_UI']='1'
env['MATTER_CAM']='425,700,1465,425,450,1365'
env['MATTER_WINDOW_WIDTH']='3840'
env['MATTER_WINDOW_HEIGHT']='2160'
env['MATTER_ASSET_BROWSER_SKIP_CACHE_STATUS']='1'
env['MATTER_VT_EVENT_LOG']='1'
env['MATTER_STREAM_PARK_PROFILE']='1'
commands=['render_path raster','set render.gi.enabled false','set render.pom.enabled false','set render.cloud_shadows.enabled false','wait_event bake.finished 120','wait_idle 10','stats travel_start']
for i in range(1,33):
 x=425; y=700; z=1465-i*128
 commands += [f'cam {x} {y} {z} {x} {y-250} {z-100}']
 if i==1:
  for f in range(6): commands += ['wait_frames 1',f'shot_now C:/tmp/matter-blas-mountain/stationary-churn-handoff-v7/transition_{f}.png']
 commands += ['wait_frames 120']
 if i%2==0:
  for j in range(8):
   angle=j*math.pi/4
   commands += [f'cam {x} {y} {z} {x+100*math.sin(angle)} {y-250} {z-100*math.cos(angle)}','wait_frames 60']
 commands += [f'stats flight_{i}']
 if i%4==0: commands += [f'shot_now C:/tmp/matter-blas-mountain/stationary-churn-handoff-v7/flight_{i}.png']
commands += ['stats stationary_0']
for i in range(1,7):commands += ['wait_frames 600',f'stats stationary_{i}']
commands += ['shot_now C:/tmp/matter-blas-mountain/stationary-churn-handoff-v7/final.png','quit']
(out/'commands.txt').write_text('\n'.join(commands)+'\n')
(out/'environment.json').write_text(json.dumps({k:v for k,v in env.items() if k.startswith('MATTER_')},indent=2))
with (out/'editor.log').open('w') as log:
 p=subprocess.Popen([str(root/'MatterEditor/build/windows-msvc/editor.exe')],cwd=root/'MatterEditor',env=env,stdout=log,stderr=subprocess.STDOUT)
 (out/'pid.txt').write_text(str(p.pid))
 try:code=p.wait(timeout=1200)
 except subprocess.TimeoutExpired:
  with (out/'commands.txt').open('a') as f:f.write('quit\n')
  try:code=p.wait(timeout=20)
  except subprocess.TimeoutExpired:p.terminate();code=p.wait()
print('exit',code)
raise SystemExit(code)
