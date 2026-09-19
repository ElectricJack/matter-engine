from pathlib import Path
import subprocess,hashlib,json,time
repo=Path('/mnt/d/Shared With Desktop/AI/matter-engine-cpp')
out=repo/'docs/agent/evidence/2026-09-15-direct-source'
base=Path('/mnt/d/tmp/matter-vt/20260915-direct-source')
source=json.loads((out/'source-integration-source.json').read_text())['sources']
for p in (repo/'projects/world_demo/scenes/texturing/bricks/ProceduralBrickProof').rglob('*.js'):
    source[str(p.relative_to(repo))]=hashlib.sha256(p.read_bytes()).hexdigest()
binary=repo/'MatterEditor/build/windows-msvc/editor.exe'
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
manifest={'sources':source,'binary_sha256':sha(binary)}
(out/'brick-receiver-source.json').write_text(json.dumps(manifest,indent=2)+'\n')
(base/'brick-commands.txt').write_text('')
command='''$env:MATTER_WORLD='ProceduralBrickProof'; $env:MATTER_HIDE_UI='1'; $env:MATTER_HIDE_WINDOW='1'; $env:MATTER_WINDOW_WIDTH='1280'; $env:MATTER_WINDOW_HEIGHT='800'; $env:MATTER_SCREENSHOT='D:/tmp/matter-vt/20260915-direct-source/brick-receiver.png'; $env:MATTER_SCREENSHOT_SETTLE='120'; $env:MATTER_CMD_FIFO='D:/tmp/matter-vt/20260915-direct-source/brick-commands.txt'; $env:MATTER_VK_VALIDATION='1'; Set-Location -LiteralPath 'D:/Shared With Desktop/AI/matter-engine-cpp/MatterEditor'; & './build/windows-msvc/editor.exe'; Write-Output "native_exit=$LASTEXITCODE"; exit $LASTEXITCODE'''
cmd=['/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe','-NoProfile','-NonInteractive','-Command',command]
start=time.monotonic()
with (out/'brick-receiver.log').open('w') as log:
    r=subprocess.run(cmd,cwd=repo,stdout=log,stderr=subprocess.STDOUT)
result={'command':cmd,'exit':r.returncode,'seconds':time.monotonic()-start,'source_changes':[p for p,h in source.items() if sha(repo/p)!=h],'binary_changed':sha(binary)!=manifest['binary_sha256'],'image_exists':(base/'brick-receiver.png').exists()}
(out/'brick-receiver-result.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result),flush=True)
raise SystemExit(r.returncode)
