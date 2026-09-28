"""Compare audited before/after captures, including POM-off cost controls."""
import json,re,statistics
from pathlib import Path
import numpy as np
from PIL import Image
root=Path(__file__).resolve().parent
labels=('v1','v2')
folders={'v1':root.parent/'2026-09-17-pom-footprint-reuse/v1','v2':root/'v1'}
audits={label:json.loads((folders[label]/'audit.json').read_text()) for label in labels}
result={'folders':{k:str(v.relative_to(root.parent)) for k,v in folders.items()},'views':{},'isolation':audits['v1']['isolation']}
for a in audits.values():
 assert a['editor_exit']==0 and a['binary_unchanged']
 assert not any(a[k] for k in ('source_changes','missing_shots','validation_errors','command_failures'))
for view in ('overview','grazing','close'):
 row={'timings':{},'residency':{},'images':{}}
 for label,a in audits.items():
  entry={}
  states=[]
  for phase in ('on-a','off','on-b'):
   timings=[float(s.split(',')[-1]) for s in a['stats'] if s.startswith(f'STATS,{view}-{phase}-')]
   entries=[s for s in a['stats'] if s.startswith(f'STATSVT,{view}-{phase}-')]
   assert len(timings)==len(entries)==30
   entry[phase]={'median_ms':statistics.median(timings),'p95_ms':float(np.percentile(timings,95))}
   for s in entries:
    assert all(t in s for t in ('active=1,','queue=0,','rejected=0,','evictions=0,'))
    states.append(tuple(re.search(pattern,s)[1] for pattern in (r'variants=(\d+)/',r'pool=(\d+)/',r'ind=([\d.]+)/')))
  assert len(set(states))==1,(label,view,set(states))
  entry['combined_on_median_ms']=statistics.median([float(s.split(',')[-1]) for s in a['stats'] if s.startswith((f'STATS,{view}-on-a-',f'STATS,{view}-on-b-'))])
  entry['pom_increment_ms']=entry['combined_on_median_ms']-entry['off']['median_ms']
  row['timings'][label]=entry;row['residency'][label]=states[0]
 assert row['residency']['v1']==row['residency']['v2'],row['residency']
 for mode in ('lit','on-again','flat','albedo','normal'):
  images=[np.asarray(Image.open(folders[label]/f'{view}-{mode}.png').convert('RGB'),dtype=np.int16) for label in labels]
  delta=np.abs(images[0]-images[1])
  crop=delta if view=='close' else delta[520:800,128:1152] if view=='overview' else delta[460:800]
  row['images'][mode]={}
  for name,d in (('full',delta),('terrain',crop)):
   row['images'][mode][name]={'mean_abs_8bit':float(d.mean()),'max_abs_8bit':int(d.max()),'pixels_above_1':int((d.max(axis=2)>1).sum())}
 old=row['timings']['v1']['combined_on_median_ms'];new=row['timings']['v2']['combined_on_median_ms']
 row['combined_on_reduction_percent']=100*(1-new/old)
 result['views'][view]=row
(root/'comparison.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result,indent=2))
