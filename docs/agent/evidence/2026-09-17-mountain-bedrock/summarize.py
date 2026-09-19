"""Read native evidence; never alter capture pixels or render inputs."""
import json, re, sys
from pathlib import Path
import numpy as np
from PIL import Image
root=Path(__file__).resolve().parent
label=sys.argv[1]
assert re.fullmatch(r'v[0-9]+',label)
run=root/label
report={'capture':label,'pom_pairs':{}}
for flat in run.glob('*-flat.png'):
 view=flat.stem.removesuffix('-flat')
 on=np.asarray(Image.open(run/f'{view}-lit.png').convert('RGB')).astype(float)
 off=np.asarray(Image.open(flat).convert('RGB')).astype(float)
 delta=np.abs(on-off)
 report['pom_pairs'][view]={'mean_abs_8bit':float(delta.mean()),'max_abs_8bit':float(delta.max()),
  'fraction_pixels_above_3_levels':float((delta.max(axis=2)>3).mean()),
  'caveat':'Lit frames also include temporal lighting variation; metrics are not a POM correctness oracle.'}
trace=json.loads((run/'bake-trace.json').read_text())['root']
report['root_bake_ms']=trace['duration_ms']
report['root_children_ms']={c['name']:c['duration_ms'] for c in trace['children']}
text=(run/'log.txt').read_text(errors='replace')
report['stream_summaries']=[l for l in text.splitlines() if re.search(r'\d+ sectors.*\d+\.\d+',l)]
report['render_timings']=json.loads((run/'audit.json').read_text())['timings']
(root/f'{label}-summary.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
