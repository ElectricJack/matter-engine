"""Run the terrain POM comparison and retain the exact source/runtime evidence."""
import hashlib,json,shutil,subprocess,time
from pathlib import Path
repo=Path(__file__).resolve().parents[4]
out=Path(__file__).resolve().parent
prefix='mountain-pom-v2'
manifest=json.loads((out.parent/'2026-09-16-shared-vt-pixels'/f'{prefix}-sources.json').read_text())
sha=lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
assert not [p for p,h in manifest.items() if sha(repo/p)!=h]
props=repo/'projects/world_demo/scenes/streaming/StreamMountain/props.json'
original_props=props.read_bytes()
(out/'original-props.json').write_bytes(original_props)
editor=repo/'MatterEditor/build/windows-msvc/editor.exe'
binary=sha(editor)
command='''Get-ChildItem Env:MATTER_* | Remove-Item; & py -3 MatterEngine3/tools/drive.py --world StreamMountain --timeline "D:/Shared With Desktop/AI/matter-engine-cpp/docs/agent/evidence/2026-09-17-mountain-pom/capture-v2.txt" --out-dir C:/tmp/matter-mountain-pom-v2 --editor "D:/Shared With Desktop/AI/matter-engine-cpp/MatterEditor/build/windows-msvc/editor.exe" --timeout 1200 --hide-ui --env MATTER_HIDE_WINDOW=1 --env MATTER_VK_VALIDATION=1 --env MATTER_WINDOW_WIDTH=1280 --env MATTER_WINDOW_HEIGHT=800 --env MATTER_PROFILE_LOG=1 --env MATTER_BAKE_TRACE=C:/tmp/matter-mountain-pom-v2/bake-trace.json; exit $LASTEXITCODE'''
start=time.monotonic()
with (out/'driver-v2.log').open('w') as log:
    result=subprocess.run(['/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe','-NoProfile','-Command',command],cwd=repo,stdout=log,stderr=subprocess.STDOUT)
# Runtime property toggles may save the world properties. Preserve the actual
# saved file as evidence, then restore the user's original scene configuration.
(out/'saved-props-after-capture.json').write_bytes(props.read_bytes())
props.write_bytes(original_props)
destination=out/'v2';assert not destination.exists()
shutil.copytree('/mnt/c/tmp/matter-mountain-pom-v2',destination)
for relative in ['projects/world_demo/shared-lib/mountain_surface.js','projects/world_demo/scenes/streaming/StreamMountain/StreamMountain.js']:
    shutil.copy2(repo/relative,destination/Path(relative).name)
text=(destination/'log.txt').read_text(errors='replace')
audit={'editor_exit':result.returncode,'seconds':time.monotonic()-start,
       'editor_sha256':binary,'binary_unchanged':sha(editor)==binary,
       'source_changes':[p for p,h in manifest.items() if sha(repo/p)!=h],
       'validation_errors':[line for line in text.splitlines() if 'Validation Error' in line],
       'stats':[line for line in text.splitlines() if line.startswith(('STATS,','STATSVT,'))],
       'isolation':'Frozen r2 editor PID 9056 was independently open; this is functional/visual evidence, not a performance acceptance run.',
       'files':{p.name:sha(p) for p in destination.iterdir() if p.is_file()}}
(destination/'audit.json').write_text(json.dumps(audit,indent=2)+'\n')
print(json.dumps({k:v for k,v in audit.items() if k not in ('files','stats')},indent=2),flush=True)
raise SystemExit(0 if result.returncode==0 and audit['binary_unchanged'] and not audit['source_changes'] and not audit['validation_errors'] else 1)
