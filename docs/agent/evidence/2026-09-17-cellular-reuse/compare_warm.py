"""Alternate immutable reference/reuse executables; same SPIR-V and authored fields."""
import hashlib,json,re,statistics,subprocess,time
from pathlib import Path
out=Path(__file__).resolve().parent;repo=out.parents[3]
shared=out.parent/'2026-09-16-shared-vt-pixels'
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
sources=json.loads((shared/'cellular-reuse-v5-sources.json').read_text())
changed=lambda:[p for p,h in sources.items() if sha(repo/p)!=h]
assert not changed(),changed()
shaders=json.loads((out/'shared-shader-hashes.json').read_text())
assert all(sha(repo/p)==h for p,h in shaders.items())
binary_path='MatterEditor/build/cmake/windows-msvc/relwithdebinfo/vt_compositor_tests.exe'
reference=repo/binary_path.replace('vt_compositor_tests.exe','vt_compositor_reference_tests.exe')
candidate=repo/binary_path
identities={name:json.loads((shared/f'{prefix}-build-manifest.json').read_text())['binaries'][binary_path]
 for name,prefix in [('reference','cellular-reuse-v4-reference'),('reuse','cellular-reuse-v5')]}
assert sha(reference)==identities['reference'] and sha(candidate)==identities['reuse']
records=[]
for index,(name,exe) in enumerate([('reuse',candidate),('reference',reference),('reuse',candidate),('reference',reference)]):
 log=out/f'warm-{index}-{name}.log';assert not log.exists()
 command=f'$env:MATTER_VK_VALIDATION="1"; $env:TMP="$env:LOCALAPPDATA\\Temp"; $env:TEMP=$env:TMP; & "./build/cmake/windows-msvc/relwithdebinfo/{exe.name}"; exit $LASTEXITCODE'
 start=time.monotonic()
 with log.open('w') as stream:
  result=subprocess.run(['/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe','-NoProfile','-Command',command],cwd=repo/'MatterEditor',stdout=stream,stderr=subprocess.STDOUT)
 text=log.read_text(errors='replace')
 samples=[float(x)/16 for x in re.findall(r'CELLULAR3_FILL sample=\d+ pages=16 ms=([.\d]+)',text)]
 row={'variant':name,'command':command,'exit':result.returncode,'seconds':time.monotonic()-start,
  'source_manifest':f'cellular-reuse-{"v4-reference" if name=="reference" else "v5"}-sources.json',
  'binary_sha256':sha(exe),'binary_unchanged':sha(exe)==identities[name],
  'working_source_changes':changed(),'samples_ms_per_page':samples,
  'median_ms_per_page':statistics.median(samples) if samples else None,
  'page_hashes':re.findall(r'CELLULAR3_PAGE (.*)',text),'log_sha256':sha(log),
  'passed':result.returncode==0 and 'ALL PASS' in text and 'validation errors: 0' in text}
 records.append(row);print(json.dumps(row),flush=True)
 assert row['passed'] and row['binary_unchanged'] and not row['working_source_changes'] and len(samples)==9
means={name:statistics.median([x for r in records if r['variant']==name for x in r['samples_ms_per_page']]) for name in identities}
audit={'runs':records,'median_ms_per_page':means,'reduction_percent':100*(1-means['reuse']/means['reference']),
 'all_page_hashes_equal':all(r['page_hashes']==records[0]['page_hashes'] for r in records),
 'shader_hashes':shaders,'shader_changes':[p for p,h in shaders.items() if sha(repo/p)!=h],
 'isolation':'Frozen r2 editor independently open. Fixture result, not whole-scene acceptance.'}
(out/'warm-comparison.json').write_text(json.dumps(audit,indent=2)+'\n')
print(json.dumps({k:v for k,v in audit.items() if k!='runs'},indent=2),flush=True)
assert audit['all_page_hashes_equal'] and not audit['shader_changes']
