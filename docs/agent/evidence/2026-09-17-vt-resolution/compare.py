"""Compare settled native captures without treating non-isolated timings as gates."""
import json,re,sys
from pathlib import Path
root=Path(__file__).resolve().parent
labels=sys.argv[1:] or ['v1','v2']
result={}
for label in labels:
 review=root/label/'review-audit.json'
 audit=json.loads((review if review.exists() else root/label/'audit.json').read_text())
 decoded=json.loads((root/f'{label}-analysis.json').read_text())
 log=(root/label/'log.txt').read_text(errors='replace')
 bake=re.search(r'\[bake-timing\] install=(\d+)ms world=(\d+)ms publish=(\d+)ms total=(\d+)ms',log)
 counters={name:[] for name in ['pool','pinned','queue','fills','evictions','ind','variants']}
 for line in log.splitlines():
  if not line.startswith('STATSVT,'):continue
  for name in counters:
   match=re.search(r'(?:,|^)'+name+r'=(\d+(?:\.\d+)?)',line)
   if match:counters[name].append(float(match.group(1)))
 result[label]={'editor_sha256':audit['editor_sha256'],
 'audit_pass':audit['editor_exit']==0 and audit['binary_unchanged'] and not any(audit[k] for k in ['source_changes','missing_shots','validation_errors','command_failures']),
  'material_acceptance':audit.get('material_acceptance','not separately audited'),
  'review_reason':audit.get('reason',''),
  'setup_ms':dict(zip(['install','world','publish','total'],map(int,bake.groups()))) if bake else None,
  'stream_fill_lines':[l for l in log.splitlines() if l.startswith('[stream.fill]')],
  'counter_high_water':{k:max(v) if v else None for k,v in counters.items()},
  'timings':audit['timings'],
  'center_resolution':{v:{'finest_tpm':r['regions']['center']['finest_texels_per_m'],
                          'resident_tpm':r['regions']['center']['resident_texels_per_m'],
                          'fallback_fraction':r['regions']['center']['resident_coarser_than_requested_fraction']}
                       for v,r in decoded['views'].items()}}
(root/'comparison.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result,indent=2))
