"""Create a disposable export-review project; never modify the frozen r1 pilot."""
from pathlib import Path
import hashlib, importlib.util, json, shutil, sys
repo=Path(__file__).resolve().parents[4]
old=repo/'MatterEditor/build/asset-handoff/2026-09-16-r1'
destination=repo/'MatterEditor/build/asset-export-validation'/sys.argv[1]
assert not destination.exists()
spec=importlib.util.spec_from_file_location('staging',repo/'tools/stage-windows-msvc-package.py')
staging=importlib.util.module_from_spec(spec);spec.loader.exec_module(staging)
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
manifest=json.loads((old/'snapshot.json').read_text());baseline=manifest['initial_project_sha256']
destination.mkdir(parents=True)
shutil.copytree(repo/'projects/world_demo',destination/'projects/world_demo',ignore=staging.ignored)
shutil.copytree(repo/'MatterEngine3/shared-lib',destination/'MatterEngine3/shared-lib',ignore=staging.ignored)
added={}
for p in (old/'projects/world_demo').rglob('*'):
    if not p.is_file() or any(n in p.parts for n in ('.cache','cache','node_modules')):continue
    relative=p.relative_to(old);digest=sha(p)
    if str(relative) in baseline and digest==baseline[str(relative)]:continue
    if p.suffix not in ('.js','.mjs','.json','.md'):continue
    target=destination/relative
    if target.exists() and str(relative) not in baseline and sha(target)!=digest:
        assert relative.name=='props.json', f'Conflicting asset source: {relative}'
        shutil.copy2(target,destination/'main-project-pilot-props.json')
    if target.exists() and str(relative) in baseline:assert sha(target)==baseline[str(relative)],f'Concurrent source conflict: {relative}'
    target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,target)
    assert sha(p)==digest and sha(target)==digest
    added[str(relative)]=digest
(destination/'pilot-source-copy.json').write_text(json.dumps(added,indent=2)+'\n')
# Copy the pilot's compatible source-generated cache into this disposable project.
# New engine hashes still invalidate derived data normally.
cache=old/'projects/world_demo/.cache/VillaDoricColumnStudy'
if cache.exists():shutil.copytree(cache,destination/'projects/world_demo/.cache/VillaDoricColumnStudy')
print(destination)
