import os,json,pathlib,subprocess,time
root=pathlib.Path('D:/Shared With Desktop/AI/matter-engine-cpp')
base=pathlib.Path('C:/tmp/matter-blas-mountain/blas-terrain-final')
for run in ['first','warm']:
 out=base/run;out.mkdir(exist_ok=True)
 env={k:v for k,v in os.environ.items() if not k.startswith('MATTER_')}
 env.update(json.loads(pathlib.Path('C:/tmp/matter-blas-mountain/controls-sep19-rt/environment.json').read_text()))
 env.update(MATTER_GEOMETRY_PAGES_PROFILE='1',MATTER_HIDE_UI='1',MATTER_HIDE_WINDOW='1',MATTER_WINDOW_WIDTH='1280',MATTER_WINDOW_HEIGHT='720')
 for k in ['MATTER_AGENT_RESULT_FILE']:env.pop(k,None)
 fifo=out/'commands.txt';env['MATTER_CMD_FIFO']=str(fifo)
 fifo.write_text('render_path native_rt\nset render.gi.enabled true\nset render.pom.enabled false\n')
 (out/'environment.json').write_text(json.dumps({k:v for k,v in env.items() if k.startswith('MATTER_')},indent=2))
 with (out/'editor.log').open('w') as log:
  p=subprocess.Popen([str(root/'MatterEditor/build/windows-msvc/editor.exe')],cwd=root/'MatterEditor',env=env,stdout=log,stderr=subprocess.STDOUT)
  print(run,'pid',p.pid,flush=True)
  for i in range(6):
   time.sleep(10)
   if p.poll() is not None:raise RuntimeError('Editor exited early: '+str(p.returncode))
   with fifo.open('a') as f:f.write(f'stats elapsed_{(i+1)*10}s\n')
  with fifo.open('a') as f:f.write(f'shot_now {out.as_posix()}/final.png\nquit\n')
  code=p.wait(timeout=60)
  print(run,'exit',code,flush=True)
  if code:raise RuntimeError(code)
