#!/usr/bin/env python3
import os, subprocess, time, json, hashlib
from pathlib import Path
repo=Path.cwd()
root=Path('/mnt/c/tmp/clear-ridge10-20260930')
(root/'editor-final.exe').write_bytes((repo/'MatterEditor/build/windows-msvc/editor.exe').read_bytes())
(root/'editor_sha256.txt').write_text(hashlib.sha256((root/'editor-final.exe').read_bytes()).hexdigest()+'  editor-final.exe\n')
for warmup in (45,300):
 for setup in ('static','vg'):
  out=root/f'{setup}-w{warmup}'
  env=os.environ.copy()
  env.update(PAGED_TERRAIN=str(int(setup=='vg')), PAGED_CACHE_ONLY='1', VARIANTS='pom_off', RUNS='3', WARMUP=str(warmup))
  print(time.strftime('%Y-%m-%dT%H:%M:%S%z'), 'START', out.name, flush=True)
  with (root/f'{out.name}.driver.log').open('w') as log:
   rc=subprocess.call(['tools/streammountain_attribution.sh', 'C:/tmp/clear-ridge10-20260930/'+out.name], env=env, stdout=log, stderr=subprocess.STDOUT)
  print(time.strftime('%Y-%m-%dT%H:%M:%S%z'), 'FINISH', out.name, 'exit',rc, flush=True)
  (root/f'{out.name}.exit.json').write_text(json.dumps({'exit_code':rc,'finished_at':time.time()})+'\n')
  if rc: raise SystemExit(rc)
print('ALL TWELVE CURRENT-BUILD CAPTURES COMPLETE',flush=True)
