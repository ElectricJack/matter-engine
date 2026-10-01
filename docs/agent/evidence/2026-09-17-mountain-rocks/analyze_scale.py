"""Measure a fixed central surface region in the physical rock size proof."""
import hashlib,json,sys
from pathlib import Path
import numpy as np
from PIL import Image

folder=Path(sys.argv[1])
def pixels(name):
    return np.asarray(Image.open(folder/name).convert('RGB'),dtype=float)[300:500,540:740]
def quantiles(values):
    return dict(zip(['p05','p50','p95'],map(float,np.percentile(values,[5,50,95]))))
result={'region_xyxy':[540,300,740,500],
 'limits':'Fixed central surface region, checked against images. Density is the ordinary proxy chart diagnostic, not the final displaced address; 8-bit PNG density quantization is about 3.3%. Image changes demonstrate an effect, not correct displacement or visual acceptance.',
 'views':{},'visual_approval':False}
for size in ['0p5','2','8','32']:
 for angle in ['near','grazing']:
    view=f'size-{size}-{angle}'
    density=pixels(f'{view}-density.png')
    valid=(density[:,:,2]==255)&(density[:,:,1]<=density[:,:,0]+1)
    row={'valid_proxy_pixels':int(valid.sum()),'region_pixels':int(valid.size)}
    if valid.any():
        decoded=2**(density[valid,:2]*12/255)
        row['finest_texels_per_m']=quantiles(decoded[:,0])
        row['resident_texels_per_m']=quantiles(decoded[:,1])
    albedo=pixels(f'{view}-albedo.png')
    row['albedo_mean']=albedo.mean(axis=(0,1)).tolist()
    row['albedo_stddev']=albedo.std(axis=(0,1)).tolist()
    for label,a,b in [('raster_pom','lit','flat'),('native_rt_pom','rt-lit','rt-flat'),
                      ('raster_vs_rt','lit','rt-lit')]:
        delta=np.abs(pixels(f'{view}-{a}.png')-pixels(f'{view}-{b}.png'))
        row[label]={'mean_absolute_byte_difference':float(delta.mean()),
                    'fraction_pixels_over_3_bytes':float(np.mean(delta.max(axis=2)>3))}
    result['views'][view]=row
result['images']={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in folder.glob('*.png')}
(folder/'material-analysis.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps({'views':{name:{'density':row.get('finest_texels_per_m'),
 'raster_pom':row['raster_pom'],'rt_pom':row['native_rt_pom']}
 for name,row in result['views'].items()}},indent=2))
