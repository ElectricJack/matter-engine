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
version=sys.argv[1] if len(sys.argv)>1 else 'v2'
assert version in ('v2','v3')
fracture=np.zeros(len(p))
if version=='v3':
 # A statistical profile estimate. Invert the nominal oblique domain; the
 # small weather warp is omitted. Native world probes check actual filtering.
 transform=np.array([[.893,0,.450],[-.450*.589,.808,.893*.589],[-.450*.808,-.589,.893*.808]])
 world=(p/3.2)@np.linalg.inv(transform).T
 q=world*12;floor=np.floor(q).astype(np.int64);t=q-floor
 fade=t*t*t*(t*(t*6-15)+10)
 for z in (0,1):
  for y in (0,1):
   for x in (0,1):
    bits=np.array([x,y,z]);weight=np.where(bits,fade,1-fade).prod(axis=1)
    fracture+=weight*rand(*(floor+bits).T,(317^0x85)^0xD31)
 fracture=fracture*2-1
gap=second-first;radius=.28+.22*value+(fracture*.10 if version=='v3' else 0)
crown=np.clip(np.minimum((gap-.02)*5,(radius-np.sqrt(first))*9),0,1)
present=np.clip((value-.08)/.20,0,1);present=present*present*(3-2*present)
profile=crown*present*(.65+.45*value)*(fracture*.10+.90 if version=='v3' else 1)
miss=(first<inner_first-1e-7)|(second<inner_second-1e-7)
result={'recipe':version,'samples':len(p),'sampling_seed':781,'site_seed':317,'mean':float(profile.mean()),
 'coverage':float((crown>.01).mean()),'outer_ring_fraction':float(mask.mean()),
 'inner_ring_misses':int(miss.sum()),'adversarial_points':p[miss][:8].tolist()}
Path(__file__).with_name('profile-estimate.json' if version=='v2' else 'profile-estimate-v3.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result,indent=2))
