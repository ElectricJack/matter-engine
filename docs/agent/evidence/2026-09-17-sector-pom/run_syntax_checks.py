"""Native MSVC /Zs checks only: no object, executable, shader or GPU work."""
from pathlib import Path
import hashlib,json,re,subprocess,sys,time
repo=Path(__file__).resolve().parents[4]
out=Path(__file__).resolve().parent
prefix=sys.argv[1]
assert re.fullmatch(r'[a-z0-9-]+',prefix)
assert not (out/f'{prefix}-syntax.json').exists()
def win(p):
    p=p.resolve().as_posix()
    assert p.startswith('/mnt/') and p[6]=='/'
    return p[5].upper()+':/'+p[7:]
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
lines=(repo/'MatterEditor/build/cmake/windows-msvc/relwithdebinfo/build.ninja').read_text().splitlines()
def flags(target,source):
    needle='build CMakeFiles\\'+target+'.dir\\'+source.replace('/','\\')+'.obj:'
    i=next(i for i,line in enumerate(lines) if line.startswith(needle))
    variables={}
    for line in lines[i+1:]:
        if not line.startswith('  '):break
        key,value=line.strip().split(' = ',1)
        variables[key]=value
    return '\n'.join(variables[k] for k in ('DEFINES','FLAGS','INCLUDES'))
checks=[
 ('engine-viewer','matter_engine_viewer_objects','MatterEngine3/src/matter_engine.cpp',None),
 ('engine-headless','matter_engine_core','MatterEngine3/src/matter_engine.cpp',None),
 ('renderer','matter_engine_viewer_objects','MatterEngine3/src/render/vk_scene_renderer.cpp',None),
 ('static-surface','solid_face_projection_gpu_tests','MatterEngine3/tests/solid_face_projection_gpu_tests.cpp','MatterEngine3/tests/static_surface_vt_tests.cpp'),
 ('smoke','vulkan_smoke_tests','MatterEngine3/tests/vulkan_smoke_tests.cpp',None)]
paths={source for _,_,source,replacement in checks}|{replacement for _,_,_,replacement in checks if replacement}
paths.update(['MatterEngine3/src/render/chart_static_surface.h','MatterEngine3/src/render/vt_surface_topology.h','MatterEngine3/src/render/vk_scene_renderer.h','cmake/MatterViewer.cmake'])
source_hashes={p:sha(repo/p) for p in sorted(paths)}
exe=repo/'MatterEditor/build/windows-msvc/editor.exe'
exe_hash=sha(exe)
ps=out/f'{prefix}-syntax.ps1'
module=win(repo/'tools/windows/MatterWindowsToolchain.psm1').replace("'","''")
ps.write_text("""param([string]$ResponseFile)
$ErrorActionPreference='Stop'
Import-Module '"""+module+"""' -Force
$t = Resolve-MatterWindowsToolchain -RepositoryRoot '"""+win(repo).replace("'","''")+"""'
$command = 'call "{0}" -arch=x64 -host_arch=x64 -winsdk={1} -vcvars_ver={2} && cl.exe /nologo /Zs @"{3}"' -f $t.VsDevCmd,$t.WindowsSdkVersion,$t.MsvcToolsVersion,$ResponseFile
& $env:ComSpec /d /s /c $command
exit $LASTEXITCODE
""")
runs=[]
for name,target,source,replacement in checks:
    rsp=out/f'{prefix}-{name}.rsp'
    rsp.write_text(flags(target,source)+'\n"'+win(repo/(replacement or source))+'"\n')
    log=out/f'{prefix}-syntax-{name}.log'
    start=time.monotonic()
    with log.open('w') as stream:
        result=subprocess.run(['/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe','-NoProfile','-ExecutionPolicy','Bypass','-File',win(ps),'-ResponseFile',win(rsp)],cwd=repo,stdout=stream,stderr=subprocess.STDOUT)
    row={'name':name,'source':replacement or source,'flags_from_target':target,'exit':result.returncode,'seconds':time.monotonic()-start,'log_sha256':sha(log)}
    runs.append(row)
    state={'syntax_only':True,'gpu_run':False,'source_hashes':source_hashes,'sources_unchanged':all(sha(repo/p)==h for p,h in source_hashes.items()),'editor_sha256':exe_hash,'editor_unchanged':sha(exe)==exe_hash,'runs':runs}
    (out/f'{prefix}-syntax.json').write_text(json.dumps(state,indent=2)+'\n')
    print(json.dumps(row),flush=True)
    if result.returncode:
        print('\n'.join(s for s in log.read_text(errors='replace').splitlines() if 'error' in s.lower())[-6000:],flush=True)
        sys.exit(result.returncode)
assert state['sources_unchanged'] and state['editor_unchanged']
