"""Verify the active renderer at each screenshot from actual editor receipts."""
import hashlib,json,re,sys
from pathlib import Path

folder=Path(sys.argv[1])
log=folder/'log.txt'
mode=None;shots=[];errors=[]
for line in log.read_text(errors='replace').splitlines():
    accepted=re.fullmatch(r'render_path: (raster|native_rt)',line)
    if accepted:mode=accepted.group(1)
    if re.search(r'render_path:.*(?:expected|unavailable)',line):errors.append(line)
    match=re.search(r'screenshot written to (.*\.png)',line)
    if match:
        name=Path(match.group(1)).name
        expected='native_rt' if '-rt-' in name else 'raster'
        shots.append({'name':name,'expected':expected,'actual':mode,'passed':mode==expected})
names={row['name'] for row in shots}
missing=[p.name for p in folder.glob('*.png') if p.name not in names]
result={'log_sha256':hashlib.sha256(log.read_bytes()).hexdigest(),'shots':shots,
        'missing_receipts':missing,'command_errors':errors,
        'passed':bool(shots) and not missing and not errors and all(row['passed'] for row in shots)}
(folder/'renderer-audit.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps({'passed':result['passed'],'shots':len(shots),
      'native_rt':sum(row['actual']=='native_rt' for row in shots),
      'mismatches':[row for row in shots if not row['passed']],'errors':errors},indent=2))
raise SystemExit(0 if result['passed'] else 1)
