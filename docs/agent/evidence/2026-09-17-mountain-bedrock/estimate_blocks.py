"""Estimate the isolated-stone profile mean; not a renderer correctness oracle."""
import json,sys
from pathlib import Path
import numpy as np
rng=np.random.default_rng(781);p=rng.uniform(-32,32,(131072,3)).astype(np.float32)
cell=np.floor(p).astype(np.int64);f=p-cell
first=np.full(len(p),100.);second=first.copy();value=np.zeros(len(p))
def rand(x,y,z,seed):
 h=(x.astype(np.uint64)*374761393+y.astype(np.uint64)*3266489917+z.astype(np.uint64)*668265263+np.uint64(seed)*2246822519)&0xffffffff
 h=((h^(h>>13))*1274126177)&0xffffffff;h^=h>>16
 return (h&0xffffff).astype(float)/16777216

def visit(x,y,z,mask):
 global first,second,value
 c=cell[mask]+np.array([x,y,z]);d=np.zeros(len(c))
 for axis,seed in enumerate([317,317^0x9e37,317^0x7f4a]):
  q=np.array([x,y,z])[axis]+rand(*c.T,seed)-f[mask,axis];d+=q*q
 a=first[mask];b=second[mask];win=d<a
 second[mask]=np.where(win,a,np.minimum(b,d));first[mask]=np.minimum(a,d)
 value[mask]=np.where(win,rand(*c.T,317^0xa511e9b3),value[mask])
mask=np.ones(len(p),dtype=bool)
for z in range(-1,2):
 for y in range(-1,2):
  for x in range(-1,2):visit(x,y,z,mask)
outside=1+np.minimum(f,1-f).min(axis=1);mask=second>outside*outside
inner_first=first.copy();inner_second=second.copy()
for z in range(-2,3):
 for y in range(-2,3):
  for x in range(-2,3):
   if max(abs(x),abs(y),abs(z))==2:visit(x,y,z,mask)
version=sys.argv[1] if len(sys.argv)>1 else 'v1'
assert version in ('v1','v2')
gap=second-first
t=np.clip((gap-.015)/(.22-.015),0,1)
profile=t*t*(3-2*t) if version=='v1' else np.clip((gap-.02)*9,0,1)
result={'samples':len(p),'mean':float(profile.mean()),'fraction_in_fractures':float((profile<.5).mean()),'purpose':'Statistical guide for the block-profile mean; domain warps and fracture outline perturbation omitted.'}
Path(__file__).with_name('block-profile-estimate.json' if version=='v1' else 'block-profile-estimate-v2.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result,indent=2))
