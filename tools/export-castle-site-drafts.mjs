// Architectural draft exports before the full site compiler accepts joins.
// node tools/export-castle-site-drafts.mjs [output-directory]
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
await import('../projects/world_demo/tests/castle_shared_lib_hooks.mjs');
const {castleSitePlan,CASTLE_SITE_NAMES,CASTLE_SITE_SEEDS}=await import('../projects/world_demo/shared-lib/castle_site_programs.js');
const repo=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const output=path.resolve(process.argv[2]||path.join(repo,'build/qa/castle-sites/drafts'));
fs.mkdirSync(output,{recursive:true});
const rotate=(p,deg)=>{const a=deg*Math.PI/180,c=Math.cos(a),s=Math.sin(a);return[c*p[0]+s*p[2],p[1],-s*p[0]+c*p[2]];};
const add=(a,b)=>a.map((v,i)=>v+b[i]);
const sub=(a,b)=>a.map((v,i)=>v-b[i]);
const mul=(a,n)=>a.map(v=>v*n);
const cross=(a,b,c)=>(b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]);
function hull(points){
 const p=[...points].sort((a,b)=>a[0]-b[0]||a[1]-b[1]);
 const chain=[];
 for(const q of p){while(chain.length>1&&cross(chain.at(-2),chain.at(-1),q)<=0)chain.pop();chain.push(q);}
 const lower=chain.length;
 for(let i=p.length-2;i>=0;i--){const q=p[i];while(chain.length>lower&&cross(chain.at(-2),chain.at(-1),q)<=0)chain.pop();chain.push(q);}
 chain.pop();return chain;
}
function intersect(subject,clip){
 let result=subject;
 for(let i=0;i<clip.length;i++){
  const a=clip[i],b=clip[(i+1)%clip.length],input=result;result=[];
  for(let j=0;j<input.length;j++){
   const p=input[j],q=input[(j+1)%input.length],cp=cross(a,b,p),cq=cross(a,b,q);
   if(cp>=-1e-8)result.push(p);
   if((cp>=0)!==(cq>=0)){const t=cp/(cp-cq);result.push([p[0]+t*(q[0]-p[0]),p[1]+t*(q[1]-p[1])]);}
  }
 }return result;
}
const area=p=>Math.abs(p.reduce((a,v,i)=>{const q=p[(i+1)%p.length];return a+v[0]*q[1]-v[1]*q[0];},0))/2;
const colors={keep:'#cab39a',hall:'#c6ccdb',chapel:'#d2bcda',service:'#bdcdbb'};
for(let index=0;index<CASTLE_SITE_NAMES.length;index++){
 const name=CASTLE_SITE_NAMES[index],site=castleSitePlan(name,CASTLE_SITE_SEEDS[index]);
 const wings=new Map(),issues=[];
 const local=(w,ref)=>{
  const s=w.plan.wingProgram.sockets.find(s=>s.sourceId===ref.portal&&s.levelId===ref.level);
  if(!s)throw Error('Missing socket '+w.id+':'+ref.portal);return s;
 };
 const socket=ref=>{
  const w=wings.get(ref.wing),s=local(w,ref);
  return {...s,center:add(rotate(s.center,w.frame.yawDeg),w.frame.origin),outward:rotate(s.outward,w.frame.yawDeg)};
 };
 for(const source of site.wings){
  let frame=source.frame;
  if(!frame){
   const p=source.placement,target=socket(p.relativeTo),own=local(source,p.socket);
   const tangent=[-target.outward[2],0,target.outward[0]];
   const targetPoint=add(add(target.center,mul(target.outward,p.outset)),mul(tangent,p.lateral));
   frame={yawDeg:p.yawDeg,origin:sub(targetPoint,rotate(own.center,p.yawDeg))};
  }
  const w={...source,frame},program=w.plan.wingProgram;
  w.point=(x,z)=>{const p=add(rotate([x,0,z],frame.yawDeg),frame.origin);return[p[0],p[2]];};
  w.polygon=hull([[0,0],[program.width,0],[program.width,program.depth],[0,program.depth]].map(p=>w.point(...p)));
  wings.set(w.id,w);
 }
 const list=[...wings.values()];
 for(let i=0;i<list.length;i++)for(let j=i+1;j<list.length;j++){
  const overlap=area(intersect(list[i].polygon,list[j].polygon));
  if(overlap>1e-5)issues.push({kind:'wing-overlap',a:list[i].id,b:list[j].id,area:overlap});
 }
 const connectors=site.connections.map(c=>{
  const sockets=[socket(c.a),socket(c.b)];
  const points=sockets.flatMap(s=>{
   const t=[-s.outward[2],0,s.outward[0]],center=add(s.center,mul(s.outward,.3));
   return[-1,1].map(sign=>{const p=add(center,mul(t,sign*s.width/2));return[p[0],p[2]];});
  });
  const polygon=hull(points);
  for(const w of list){const overlap=area(intersect(polygon,w.polygon));if(overlap>1e-5)issues.push({kind:'connector-wing-overlap',connector:c.id,wing:w.id,area:overlap});}
  return{id:c.id,polygon,sockets};
 });
 const all=list.flatMap(w=>w.polygon),minX=Math.min(...all.map(p=>p[0]))-4,minZ=Math.min(...all.map(p=>p[1]))-4;
 const maxX=Math.max(...all.map(p=>p[0]))+4,maxZ=Math.max(...all.map(p=>p[1]))+4;
 const width=maxX-minX,height=maxZ-minZ;
 const points=p=>p.map(v=>v.map(n=>n.toFixed(4)).join(',')).join(' ');
 const lines=[`<svg xmlns="http://www.w3.org/2000/svg" viewBox="${minX} ${minZ-5} ${width} ${height+7}" width="1000" height="${Math.round(1000*(height+7)/width)}">`,
  `<rect x="${minX}" y="${minZ-5}" width="${width}" height="${height+7}" fill="#f5f2e9"/>`,
  `<text x="${minX+1}" y="${minZ-2.5}" font-size="1.5" font-family="sans-serif">${name.replaceAll('-',' ').toUpperCase()}</text>`,
  `<text x="${minX+1}" y="${minZ-.5}" font-size=".8" font-family="sans-serif">Architectural draft — connector clearance and geometry pending</text>`];
 for(const c of connectors)lines.push(`<polygon points="${points(c.polygon)}" fill="#ded7c7" stroke="#806f51" stroke-width=".15"/>`);
 for(const w of list){
  const p=w.plan.wingProgram;
  lines.push(`<polygon points="${points(w.polygon)}" fill="${colors[p.kind]}" stroke="#343b41" stroke-width=".22"/>`);
  for(let x=1;x<p.width;x++)lines.push(`<polyline points="${points([w.point(x,0),w.point(x,p.depth)])}" fill="none" stroke="#ffffff" opacity=".25" stroke-width=".06"/>`);
  for(let z=1;z<p.depth;z++)lines.push(`<polyline points="${points([w.point(0,z),w.point(p.width,z)])}" fill="none" stroke="#ffffff" opacity=".25" stroke-width=".06"/>`);
  for(const room of w.plan.levels[0].rooms){if(!room.rect)continue;const r=room.rect;lines.push(`<polygon points="${points([[r.x,r.z],[r.x+r.width,r.z],[r.x+r.width,r.z+r.depth],[r.x,r.z+r.depth]].map(q=>w.point(...q)))}" fill="none" stroke="#51535b" stroke-width=".11"/>`);}
  const center=w.point(p.width/2,p.depth/2);
  lines.push(`<text x="${center[0]}" y="${center[1]}" text-anchor="middle" font-family="sans-serif" font-size="1">${w.id}</text>`,
   `<text x="${center[0]}" y="${center[1]+1.2}" text-anchor="middle" font-family="sans-serif" font-size=".7">${w.frame.yawDeg}° · ${p.storeys} storeys</text>`);
 }
 for(const c of connectors)for(const s of c.sockets){const t=[-s.outward[2],s.outward[0]],p=[s.center[0],s.center[2]];lines.push(`<path d="M ${p[0]-t[0]} ${p[1]-t[1]} L ${p[0]+t[0]} ${p[1]+t[1]}" stroke="#f5f2e9" stroke-width=".45"/>`);}
 lines.push('</svg>');
 fs.writeFileSync(path.join(output,name+'.svg'),lines.join('\n'));
 fs.writeFileSync(path.join(output,name+'.json'),JSON.stringify({draft:true,site,wings:list.map(({id,frame,polygon})=>({id,frame,polygon})),connectors,issues},null,2)+'\n');
 console.log(JSON.stringify({name,wings:list.length,connectors:connectors.length,issues}));
}
