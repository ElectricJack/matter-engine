// Independent algebra gate; actual shader and raster/RT agreement is exercised
// by the visible native vt-normal-frame smoke mode.
import assert from 'node:assert/strict';
const add=(a,b)=>a.map((x,i)=>x+b[i]);
const mul=(a,s)=>a.map(x=>x*s);
const dot=(a,b)=>a.reduce((s,x,i)=>s+x*b[i],0);
const cross=(a,b)=>[a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]];
const norm=a=>mul(a,1/Math.sqrt(dot(a,a)));
const apply=(m,v)=>m.map(r=>dot(r,v));
const frame=n=>{n=norm(n);const a=Math.abs(n[0])>.999?[0,0,1]:[1,0,0];const t=norm(add(a,mul(n,-dot(a,n))));return[t,cross(n,t),n];};
const encode=(n,v)=>frame(n).map(a=>dot(a,v));
const decode=(n,v)=>norm(frame(n).reduce((s,a,i)=>add(s,mul(a,v[i])),[0,0,0]));
const distance=(a,b)=>Math.hypot(...a.map((x,i)=>x-b[i]));
const rotation=(axis,degrees)=>{const a=norm(axis),t=degrees*Math.PI/180,c=Math.cos(t),s=Math.sin(t);return a.map((_,i)=>a.map((__,j)=>c*(i===j?1:0)+(1-c)*a[i]*a[j]+s*[[0,-a[2],a[1]],[a[2],0,-a[0]],[-a[1],a[0],0]][i][j]));};
const ns=[[1,0,0],[-1,0,0],[0,1,0],[0,-1,0],[0,0,1],[0,0,-1],norm([1,.0001,0]),norm([.999,.0447,0]),norm([.9989,.047,0]),norm([.2,.7,.4])];
const directional=norm([.35,-.2,1]);let cases=0;
for(const n of ns){
  const source=decode(n,directional);
  assert.ok(distance(encode(n,source),directional)<1e-12);
  assert.ok(distance(source,n)>.1,'principal-axis detail must survive');
  assert.ok(distance(decode(n,[0,0,1]),n)<1e-12);
  for(const angle of [0,15,45,90]) for(const axis of [[0,1,0],[1,0,0],[0,0,1],[1,2,3]]) for(const scale of [[1,1,1],[2,.7,1.3],[-1,1,1]]) {
    const r=rotation(axis,angle);
    // N=R*inverse(S), the exact inverse transpose of M=R*S.
    const N=r.map(row=>row.map((x,j)=>x/scale[j]));
    const expected=norm(apply(N,source));
    const basis=frame(n).map(v=>apply(N,v));
    const hit=norm(basis.reduce((s,a,i)=>add(s,mul(a,directional[i])),[0,0,0]));
    const raster=norm(apply(N,decode(n,directional)));
    assert.ok(distance(hit,expected)<1e-12);
    assert.ok(distance(raster,expected)<1e-12);
    const back=norm(mul(basis.reduce((s,a,i)=>add(s,mul(a,directional[i])),[0,0,0]),-1));
    assert.ok(distance(back,mul(expected,-1))<1e-12);
    cases++;
  }
}
// Regression must discriminate the old world-axis reconstruction.
const n=[0,1,0],r=rotation([0,1,0],90);
assert.ok(distance(decode(apply(r,n),directional),apply(r,decode(n,directional)))>.3);
console.log(`VT normal-frame reference: ${cases} transformed directional probes passed; neutral, +/-X, reflection and back-face checks passed.`);
