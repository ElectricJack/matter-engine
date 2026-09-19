"""Freeze a new asset-export runtime, preserving r1 and its authored pilot sources."""
from pathlib import Path
import hashlib, importlib.util, json, shutil, subprocess, sys
repo=Path(__file__).resolve().parents[4]
evidence=Path(__file__).resolve().parent
checks=evidence.parent/'2026-09-16-shared-vt-pixels'
prefix=sys.argv[1]
id='2026-09-16-r2'
destination=repo/'MatterEditor/build/asset-handoff'/id
old=destination.parent/'2026-09-16-r1'
assert not destination.exists(), 'Never overwrite frozen handoffs'
def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
build=json.loads((checks/f'{prefix}-build-manifest.json').read_text())
assert all(r['exit']==0 for r in build['builds']) and not build['source_changes']
assert all(sha(repo/p)==h for p,h in build['sources'].items())
binary='MatterEditor/build/windows-msvc/editor.exe'
assert sha(repo/binary)==build['binaries'][binary]
spec=importlib.util.spec_from_file_location('staging',repo/'tools/stage-windows-msvc-package.py')
staging=importlib.util.module_from_spec(spec);spec.loader.exec_module(staging)
destination.mkdir(parents=True)
for f in ['editor.exe','editor.pdb']:shutil.copy2(repo/'MatterEditor/build/windows-msvc'/f,destination/f)
shutil.copytree(repo/'projects/world_demo',destination/'projects/world_demo',ignore=staging.ignored)
shutil.copytree(repo/'MatterEngine3/shared-lib',destination/'MatterEngine3/shared-lib',ignore=staging.ignored)
# Preserve the asset agent's additions to the original frozen project. Check all
# collisions; retain the main-checkout props preimage when the pilot differs.
baseline=json.loads((old/'snapshot.json').read_text())['initial_project_sha256'];pilot={}
for p in (old/'projects/world_demo').rglob('*'):
    if not p.is_file() or any(n in p.parts for n in ('.cache','cache','node_modules')):continue
    if p.suffix not in ('.js','.mjs','.json','.md'):continue
    relative=p.relative_to(old);key=relative.as_posix();digest=sha(p)
    if baseline.get(key)==digest:continue
    target=destination/relative
    if target.exists() and sha(target)!=digest:
        if key in baseline:assert sha(target)==baseline[key],f'Concurrent source conflict: {relative}'
        else:
            assert relative.name=='props.json',f'Conflicting new asset: {relative}'
            saved=destination/'validation/source-preimages'/relative;saved.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(target,saved)
    target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,target)
    assert sha(p)==digest and sha(target)==digest
    pilot[key]=digest
(destination/'MatterEditor').mkdir()
for relative in ['tools/matter_agent.py','MatterEngine3/tools/drive.py','docs/agent/control-surface.md',
                 'docs/agent/agent-protocol.md','docs/agent/asset-export.md','MatterEngine3/docs/authoring.md']:
    target=destination/relative;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(repo/relative,target)
notices=['MatterEngine third-party notices\n']
for name,relative,mode in staging.NOTICE_COMPONENTS:notices.append(f'\n===== {name}: {relative} =====\n{staging.read_notice(repo/relative,mode)}\n')
(destination/'THIRD_PARTY_NOTICES.txt').write_text(''.join(notices))
for f in ['launch.ps1','launch.cmd','verify-runtime.ps1']:shutil.copy2(old/f,destination/f)
p=destination/'launch.ps1';p.write_text(p.read_text().replace("[string]$World = 'ClayBrickWallSurfaceProof'", "[string]$World = 'VillaDoricColumnStudy'"))
(destination/'README.md').write_text(f'''# Frozen asset-authoring build: {id}

Double-click **launch.cmd** to open the marble column pilot with the editor panels.
`launch.ps1 -World ClayBrickMaze` opens the brick maze. F11 toggles presentation.
This new version adds static mesh/material export. The previous r1 is untouched.

## Export

Open an asset in **Bake Lab → Workbench**, finish its bake and use **Export asset**.
Choose a new folder and mesh LOD, then click **Export OBJ + GLB + textures**.
The status reports success or a specific error. Existing folders are protected.

Export creates `asset.obj`, `asset.mtl`, a self-contained `asset.glb`, conventional
PBR PNG maps, a 16-bit height PNG and `manifest.json`. GLB is the recommended
portable material preview. Height is supplied separately; standard GLB viewers
do not reproduce Matter POM. Read **docs/agent/asset-export.md** for conventions,
legacy height datum caveats and unsupported material/geometry features.

Agents can export the pilot's actual scene parameters with `asset.export`:

```json
{{"source":"world","module":"VillaDoricColumnPilot","lod":0,"directory":"D:/asset-exports/column-01"}}
```

Use the included `tools/matter_agent.py` with a running editor's command/result
files. Start with `launch.ps1 -CommandFile C:/tmp/asset-commands.txt -ResultFile
C:/tmp/asset-results.jsonl` (on one line). Paths sent to Windows must be Windows
paths. `source:workbench` exports the current open Workbench part; `part_hash`
selects a specific published world-root variant when its module is ambiguous.

## Keep the runtime fixed, edit the project

Create assets in this folder's **projects/world_demo/** copy. The r1 pilot JS
additions are included, with hashes in `snapshot.json`. New grouped objects belong
in `objects/<subject>/`, scenes in `scenes/<subject>/`, and project helpers in
`shared-lib/`. Module/class names must be unique across the project.

The default texture density is 512 texels/metre. `-TextureDensity 256` gives a
lighter preview. Caches start cold; the first column bake can take about a minute.
Export is offline and pauses the editor while all material pages are composed
and files are written. It does not depend on camera-visible VT pages.

Normal development rebuilds never write here. Keep `editor.exe`, symbols, engine
helpers, automation tools and launchers fixed. `verify-runtime.ps1` verifies them;
project edits are deliberately excluded. Copy authored source/assets back to the
main checkout when ready, preserving concurrent edits and excluding `.cache`,
workbench scratch data and generated captures. Future engine changes need a new
version directory. Continue to use r1 for any session already pinned to it.

## Scope and evidence

MSVC RelWithDebInfo development snapshot; Vulkan/RT and autoremesher enabled,
optional PhysX and Streamline disabled. This is a working-tree snapshot, not a
formal clean-tree release. `validation/` records source/binary identities, native
checks, clean-PATH startup/export checks, and independent format/viewer checks.
The broader texture appearance and VT performance goals remain active.
''')
runtime=[destination/f for f in ['editor.exe','editor.pdb','launch.ps1','launch.cmd','verify-runtime.ps1','tools/matter_agent.py','MatterEngine3/tools/drive.py']]
runtime += [p for p in (destination/'MatterEngine3/shared-lib').rglob('*') if p.is_file()]
manifest={'id':id,'kind':'frozen-development-snapshot','configuration':'MSVC RelWithDebInfo',
    'source_revision':subprocess.check_output(['git','rev-parse','HEAD'],cwd=repo,text=True).strip(),
    'source_tracked_dirty':True,'features':{'vulkan':True,'autoremesher':True,'physx':False,'streamline':False,'asset_export':True},
    'validation_prefix':prefix,'launch_defaults':{'world':'VillaDoricColumnStudy','prop_texture_density':512,'editor_panels_visible':True},
    'runtime_sha256':{p.relative_to(destination).as_posix():sha(p) for p in runtime},
    'initial_project_sha256':{p.relative_to(destination).as_posix():sha(p) for p in sorted((destination/'projects/world_demo').rglob('*')) if p.is_file()},
    'r1_authored_additions_sha256':pilot,'native_sources_sha256':build['sources']}
(destination/'snapshot.json').write_text(json.dumps(manifest,indent=2)+'\n')
out=destination/'validation';out.mkdir(exist_ok=True)
for tag in ['export-v5','export-v6',prefix]:
    for p in checks.glob(tag+'-*'):
        if p.suffix in ['.log','.json']:shutil.copy2(p,out/p.name)
for p in evidence.iterdir():
    if p.is_file() and p.suffix in ['.png','.json','.log']:shutil.copy2(p,out/p.name)
print(json.dumps({'snapshot':str(destination),'runtime_files':len(runtime),'pilot_files':len(pilot),'editor_sha256':sha(destination/'editor.exe')}),flush=True)
