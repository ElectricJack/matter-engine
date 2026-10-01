"""Decode raw proxy VT diagnostics; retain distributions rather than one pixel."""
import json, sys
from pathlib import Path
import numpy as np
from PIL import Image
root=Path(__file__).resolve().parent
label=sys.argv[1]
folder=root/label
audit=json.loads((folder/'audit.json').read_text())
views=list(audit['timings'])
assert views and all(v in ('overview','grazing','cliff','cliff-grazing','close') for v in views)
result={'label':label,'encoding':'mips / 8; log2(world texels/metre) / 12; blue valid',
        'limits':'Proxy coordinate, ordinary chart pages; conservative longest world texel edge. Quantized PNG, about 3.3% density steps. Reported density clamps to 1..4096 t/m; encoded zero means <=1 t/m, not exactly 1. No inference about final displaced addresses.',
        'views':{}}
def percentiles(values):
 return dict(zip(['p05','p25','p50','p75','p95'],map(float,np.percentile(values,[5,25,50,75,95])))) if values.size else None
for view in views:
 mip=np.array(Image.open(folder/f'{view}-mip.png').convert('RGB')).astype(float)
 den=np.array(Image.open(folder/f'{view}-density.png').convert('RGB')).astype(float)
 decoded=np.rint(mip[:,:,:2]*8/255).astype(int)
 valid=(mip[:,:,2]>=254)&(den[:,:,2]>=254)&(decoded[:,:,0]<=8)&(decoded[:,:,1]<=8)&(decoded[:,:,1]>=decoded[:,:,0])
 valid&=np.max(np.abs(mip[:,:,:2]-decoded*255/8),axis=2)<=1.1
 valid&=~((den[:,:,0]>253)&(den[:,:,1]<2))
 row={'diagnostic_pixels':int(valid.sum()),'fraction_of_frame':float(valid.mean()),'regions':{}}
 for name,(y0,y1,x0,x1) in {'whole':(0,1,0,1),'center':(.25,.75,.25,.75),'bottom':(.75,1,.25,.75)}.items():
  h,w=valid.shape;region=np.zeros_like(valid);region[int(y0*h):int(y1*h),int(x0*w):int(x1*w)]=True
  mask=valid&region
  if not mask.any():continue
  pair=decoded[mask];density=2**(12*den[mask,:2]/255)
  unique,counts=np.unique(pair,axis=0,return_counts=True)
  row['regions'][name]={'pixels':int(mask.sum()),'mip_pairs':{f'{a}->{b}':int(n) for (a,b),n in zip(unique,counts)},
    'finest_texels_per_m':percentiles(density[:,0]),'resident_texels_per_m':percentiles(density[:,1]),
    'resident_coarser_than_requested_fraction':float(np.mean(pair[:,1]>pair[:,0])),
    'finest_density_clipped_low_fraction':float(np.mean(den[mask,0]==0)),
    'resident_density_clipped_low_fraction':float(np.mean(den[mask,1]==0))}
 reference=Path(sys.argv[2]) if len(sys.argv)>2 else root.parent/'2026-09-17-mountain-layering'/'v4'
 old=reference/f'{view}-lit.png'
 if old.exists():
  delta=np.abs(np.array(Image.open(old).convert('RGB')).astype(int)-np.array(Image.open(folder/f'{view}-lit.png').convert('RGB')).astype(int))
  row['lit_vs_previous_binary']={'mean_absolute_byte_error':float(delta.mean()),'fraction_pixels_over_3_bytes':float(np.mean(delta.max(axis=2)>3))}
 row['raw_channel_comparison']={}
 for channel in ['albedo','normal']:
  old=reference/f'{view}-{channel}.png'
  if not old.exists():continue
  delta=np.abs(np.array(Image.open(old).convert('RGB')).astype(int)-np.array(Image.open(folder/f'{view}-{channel}.png').convert('RGB')).astype(int))
  row['raw_channel_comparison'][channel]={'mean_absolute_byte_error':float(delta.mean()),'fraction_pixels_over_3_bytes':float(np.mean(delta.max(axis=2)>3)),'max_byte_error':int(delta.max())}
 row['timing']=audit['timings'][view]
 result['views'][view]=row
(root/f'{label}-analysis.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result,indent=2))
