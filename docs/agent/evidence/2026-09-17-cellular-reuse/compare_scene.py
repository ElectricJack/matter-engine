"""Compare native before/after screenshots; do not synthesize replacement images."""
import json
from pathlib import Path
import numpy as np
from PIL import Image
out=Path(__file__).resolve().parent
root=out.parent/'2026-09-17-mountain-cellular'
before=root/'v3';after=root/'v4'
audit=json.loads((after/'audit.json').read_text())
assert audit['editor_exit']==0 and audit['binary_unchanged']
assert not any(audit[k] for k in ('source_changes','missing_shots','validation_errors','command_failures'))
rows=[]
for view in ('overview','grazing','close'):
 for mode in ('lit','albedo','normal'):
  a=np.asarray(Image.open(before/f'{view}-{mode}.png').convert('RGB'),dtype=np.int16)
  b=np.asarray(Image.open(after/f'{view}-{mode}.png').convert('RGB'),dtype=np.int16)
  assert a.shape==b.shape
  d=np.abs(a-b)
  yy,xx=np.where(np.any(d>1,axis=2))
  rows.append({'view':view,'mode':mode,'max_channel_difference_8bit':int(d.max()),
    'mean_channel_difference_8bit':float(d.mean()),
    'percent_pixels_different':float(np.any(d!=0,axis=2).mean()*100),
    'percent_pixels_more_than_one_level':float(np.any(d>1,axis=2).mean()*100),
    'over_one_level_bbox_xyxy':[int(xx.min()),int(yy.min()),int(xx.max()),int(yy.max())] if len(yy) else None,
    'foreground_y460_max_difference_8bit':int(d[460:].max()) if view=='grazing' else None})
result={'before':str(before),'after':str(after),'comparisons':rows,
 'note':'Lit differences include unfrozen temporal lighting. Pixel metrics do not establish realism or performance acceptance.'}
(out/'scene-comparison.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result,indent=2))
