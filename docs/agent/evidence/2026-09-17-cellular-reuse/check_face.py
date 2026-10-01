"""Check the other consumer of the shared surface evaluator, against frozen sources."""
import hashlib,json,subprocess,sys,time
from pathlib import Path
out=Path(__file__).resolve().parent;repo=out.parents[3]
prefix=sys.argv[1]
sources=json.loads((out.parent/'2026-09-16-shared-vt-pixels'/f'{prefix}-sources.json').read_text())
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
changed=lambda:[p for p,h in sources.items() if sha(repo/p)!=h]
assert not changed(),changed()
binary=repo/'MatterEditor/build/cmake/windows-msvc/relwithdebinfo/solid_face_projection_gpu_tests.exe'
identity=sha(binary);log=out/f'{prefix}-face.log';assert not log.exists()
command='$env:MATTER_VK_VALIDATION="1"; $env:TMP="$env:LOCALAPPDATA\\Temp"; $env:TEMP=$env:TMP; & "./MatterEditor/build/cmake/windows-msvc/relwithdebinfo/solid_face_projection_gpu_tests.exe"; exit $LASTEXITCODE'
start=time.monotonic()
with log.open('w') as stream:
 result=subprocess.run(['/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe','-NoProfile','-Command',command],cwd=repo,stdout=stream,stderr=subprocess.STDOUT)
text=log.read_text(errors='replace')
audit={'command':command,'exit':result.returncode,'seconds':time.monotonic()-start,
 'binary_sha256':identity,'binary_unchanged':sha(binary)==identity,'source_changes':changed(),
 'pass_marker':'solid_face_projection_gpu_tests: PASS' in text,
 'validation_errors':[s for s in text.splitlines() if 'Validation Error' in s],
 'log_sha256':sha(log)}
audit['passed']=not audit['exit'] and audit['pass_marker'] and audit['binary_unchanged'] and not audit['source_changes'] and not audit['validation_errors']
(out/f'{prefix}-face.json').write_text(json.dumps(audit,indent=2)+'\n')
print(json.dumps(audit,indent=2),flush=True)
raise SystemExit(0 if audit['passed'] else 1)
