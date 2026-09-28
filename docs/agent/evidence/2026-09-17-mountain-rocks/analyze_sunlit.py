"""Image controls for rock relief; these measurements are not visual approval."""
import hashlib,json,sys
from pathlib import Path
import numpy as np
from PIL import Image

folder=Path(sys.argv[1]);baseline=Path(sys.argv[2]) if len(sys.argv)>2 else None
def read(root,name):
    return np.asarray(Image.open(root/name).convert('RGB'),dtype=float)[300:500,540:740]
def difference(a,b):
    delta=np.abs(a-b)
    return {'mean_absolute_byte_difference':float(delta.mean()),
            'fraction_pixels_over_3_bytes':float(np.mean(delta.max(axis=2)>3))}
result={'region_xyxy':[540,300,740,500],
        'limits':'Matched fixed central image regions. Image differences are not a depth oracle; RT samples can introduce noise. Small-rock grazing views can include background and are not a pure material measurement.',
        'visual_approval':False,'views':{}}
for size in ['0p5','2','8','32']:
 for angle in ['near','grazing']:
    view=f'size-{size}-{angle}';row={}
    for renderer,prefix in [('raster',''),('native_rt','rt-')]:
        lit=read(folder,f'{view}-{prefix}lit.png');flat=read(folder,f'{view}-{prefix}flat.png')
        row[renderer]={'pom_difference':difference(lit,flat),'lit_mean_rgb':lit.mean(axis=(0,1)).tolist(),
                       'lit_stddev_rgb':lit.std(axis=(0,1)).tolist()}
        if baseline: row[renderer]['lit_vs_baseline']=difference(lit,read(baseline,f'{view}-{prefix}lit.png'))
    row['raster_vs_rt']=difference(read(folder,f'{view}-lit.png'),read(folder,f'{view}-rt-lit.png'))
    result['views'][view]=row
result['images']={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in folder.glob('*.png')}
(folder/'sunlit-analysis.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps({'views':{k:{r:v[r]['pom_difference'] for r in ['raster','native_rt']}
                         for k,v in result['views'].items()}},indent=2))
