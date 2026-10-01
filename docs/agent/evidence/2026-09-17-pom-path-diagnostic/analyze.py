"""Classify raw route colors and compare non-diagnostic terrain channel controls."""
import json,sys
from pathlib import Path
import numpy as np
from PIL import Image
root=Path(__file__).resolve().parent
run=root/(sys.argv[1] if len(sys.argv)>1 else 'v1')
audit=json.loads((run/'audit.json').read_text())
assert audit['editor_exit']==0 and audit['binary_unchanged']
assert not any(audit[k] for k in ('source_changes','missing_shots','validation_errors','command_failures'))
baseline=root.parent/'2026-09-17-pom-footprint-reuse/v1'
palette={'ordinary_hit':[0,255,0],'connected_hit':[255,255,0], 'off':[64,64,64],
 'initial_boundary':[255,128,0],'path_boundary':[255,0,0],
 'snapshot_mismatch':[255,0,255],'travel_limit':[0,0,255],
 'unresolved':[0,255,255],'unsupported':[255,255,255],'zero_depth':[0,64,0]}
result={'scope':'Successful route pixel counts, not attempted traversal work or GPU cost.',
 'tolerance_8bit':2,'palette':palette,'views':{},'timings':audit['timings'],'isolation':audit['isolation']}
for view in ('overview','grazing','close'):
 image=np.asarray(Image.open(run/f'{view}-path.png').convert('RGB'),dtype=np.int16)
 h,w=image.shape[:2]
 box=(128,520,1152,800) if view=='overview' else (0,460,1280,800) if view=='grazing' else (0,0,w,h)
 x0,y0,x1,y1=box
 row={'terrain_roi':box,'regions':{},'channel_controls':{}}
 for name,a in [('full',image),('terrain',image[y0:y1,x0:x1])]:
  masks={k:np.max(np.abs(a-np.asarray(v,dtype=np.int16)),axis=2)<=2 for k,v in palette.items()}
  counts={k:int(v.sum()) for k,v in masks.items()}
  counts['unclassified']=a.shape[0]*a.shape[1]-sum(counts.values())
  hits=counts['ordinary_hit']+counts['connected_hit']
  row['regions'][name]={'pixels':int(a.shape[0]*a.shape[1]),'counts':counts,
    'connected_share_of_successful_hits':counts['connected_hit']/hits if hits else None}
 for mode in ('albedo','normal'):
  old=np.asarray(Image.open(baseline/f'{view}-{mode}.png').convert('RGB'),dtype=np.int16)
  new=np.asarray(Image.open(run/f'{view}-{mode}.png').convert('RGB'),dtype=np.int16)
  d=np.abs(old[y0:y1,x0:x1]-new[y0:y1,x0:x1])
  row['channel_controls'][mode]={'mean_abs_8bit':float(d.mean()),'max_abs_8bit':int(d.max()),
    'pixels_above_1':int((d.max(axis=2)>1).sum())}
 result['views'][view]=row
(run/'analysis.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result,indent=2))
