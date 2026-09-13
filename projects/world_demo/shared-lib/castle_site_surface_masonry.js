// Production manifest adapter: physical wall envelopes, with brick relief
// supplied by a surface-detail material. No brick children or scaled instances.
// The plan still owns wing transforms, corner trims and all transit geometry.
import { masonryOptions, wallModuleSockets, layoutOpenBoundary } from 'shared-lib/castle_masonry';
import { makeSurfaceFace, emitSurfaceShell } from 'shared-lib/castle_surface_shells';

const EPS = 1e-7;
const unique = xs => [...new Set(xs.map(x => Math.round(x * 1e9) / 1e9))].sort((a,b) => a-b);
const cross = (a,b) => [a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]];
const sub = (a,b) => a.map((v,i) => v-b[i]);

// Convex profile extrusion. Each descriptor is closed independently, including
// internal contact faces between adjacent wall strips; no open portal caps.
function prism(id, profile, point, thickness, material, kind='wall') {
  const cleaned = profile.filter((p,i) => i===0 || Math.hypot(...sub(p,profile[i-1]))>EPS);
  if (cleaned.length>2 && Math.hypot(...sub(cleaned[0],cleaned[cleaned.length-1]))<EPS) cleaned.pop();
  const low=cleaned.map(p=>point(p[0],p[1],-thickness/2));
  const high=cleaned.map(p=>point(p[0],p[1],thickness/2));
  const faces=[];
  const add=(points,normal,label)=>faces.push(makeSurfaceFace(points,normal,id+':'+label));
  const across=sub(high[0],low[0]);
  add(low,across.map(x=>-x),'back'); add(high,across,'front');
  for(let i=0;i<low.length;i++){
    const j=(i+1)%low.length;
    add([low[i],low[j],high[j],high[i]],cross(sub(low[j],low[i]),across),'edge-'+i);
  }
  // Use a common metre frame for a wall line: repeating brick phase survives
  // module boundaries, window strips and junctions (face-centred UVs would reset).
  for(const f of faces) {
    if(Math.abs(f.normal[1])<EPS) {
      const u=[f.normal[2],0,-f.normal[0]];
      f.uv=f.positions.map(p=>[p[0]*u[0]+p[2]*u[2],p[1]]);
      f.frame={origin:[0,0,0],uAxis:u,vAxis:[0,1,0]};
    }
  }
  return {id,kind,material,faces,solid:[...low,...high]};
}

function openingProfile(socket,axisIndex,baseY) {
  const centre=socket.origin[axisIndex===0?0:2],a=centre-socket.width/2,b=centre+socket.width/2;
  let points=[[a,socket.clearTop],[b,socket.clearTop]];
  if(socket.arch) {
    const arch=socket.arch,cy=arch.centre[1]-baseY,cx=arch.centre[axisIndex===0?0:2];
    const phi=Math.asin(Math.max(-1,Math.min(1,(arch.springY-cy)/arch.radius)));
    points=[];
    for(let i=0;i<=arch.voussoirs;i++) {
      const theta=Math.PI-phi-i*(Math.PI-2*phi)/arch.voussoirs;
      points.push([cx+arch.radius*Math.cos(theta),cy+arch.radius*Math.sin(theta)]);
    }
    // Socket rounding must not leave microscopic slivers at the jambs.
    points[0]=[a,socket.clearTop];points[points.length-1]=[b,socket.clearTop];
  }
  return {socket,a,b,bottom:socket.clearBottom,points};
}
function headAt(ap,u) {
  for(let i=1;i<ap.points.length;i++) {
    const a=ap.points[i-1],b=ap.points[i];
    if(u<=b[0]+EPS) return a[1]+(b[1]-a[1])*Math.max(0,Math.min(1,(u-a[0])/(b[0]-a[0])));
  }
  return ap.points[ap.points.length-1][1];
}

export function layoutSiteSurfaceMasonry(manifest,p={}) {
  if(manifest.curves?.length) throw new RangeError('site surface masonry requires straight compiled wing walls; curved manifests need a radial adapter');
  const options=masonryOptions(p),stone=Number.isInteger(p.stoneMaterial)&&p.stoneMaterial>=0?p.stoneMaterial:options.palettes.stone[0];
  const levels=new Map(manifest.levels.map(l=>[l.id,l]));
  const lines=new Map(),sockets=[],shells=[];
  const lineKey=r=>r.levelId+':'+(r.from[1]===r.to[1]?'x:'+r.from[1]:'z:'+r.from[0]);
  for(const r of manifest.wallModules) {
    if(r.from[0]!==r.to[0]&&r.from[1]!==r.to[1]) throw new RangeError('wing wall module must be axis aligned before the site rigid transform');
    const key=lineKey(r);if(!lines.has(key))lines.set(key,new Map());
    for(const socket of wallModuleSockets(r,manifest,options)) {
      if(!lines.get(key).has(socket.apertureId))sockets.push(socket);
      lines.get(key).set(socket.apertureId,socket);
    }
  }
  for(const r of manifest.wallModules) {
    const ai=r.from[1]===r.to[1]?0:1,baseY=levels.get(r.levelId).baseY;
    const start=r.from[ai]+r.trim.start,end=r.to[ai]-r.trim.end;
    if(end-start<EPS)continue;
    const h=r.section.height,t=r.section.thickness,line=r.from[1-ai];
    const point=(u,v,w)=>ai===0?[u,baseY+v,line+w]:[line-w,baseY+v,u];
    const aps=[...lines.get(lineKey(r)).values()].map(s=>openingProfile(s,ai,baseY)).filter(a=>a.b>start+EPS&&a.a<end-EPS);
    const cuts=unique([start,end,...aps.flatMap(a=>a.points.map(p=>p[0])).filter(u=>u>start+EPS&&u<end-EPS)]);
    for(let i=1;i<cuts.length;i++) {
      const a=cuts[i-1],b=cuts[i],mid=(a+b)/2;
      const ap=aps.find(o=>mid>o.a-EPS&&mid<o.b+EPS);
      const id=r.id+':strip-'+i;
      if(!ap) { shells.push(prism(id,[[a,0],[b,0],[b,h],[a,h]],point,t,stone));continue; }
      const ha=headAt(ap,a),hb=headAt(ap,b);
      if(h-Math.min(ha,hb)>EPS) shells.push(prism(id+':head',[[a,ha],[b,hb],[b,h],[a,h]],point,t,stone));
      if(ap.bottom>EPS) {
        // Projecting sill preserves the old 5cm shoulder and clear sill height.
        const count=Math.max(1,Math.round(h/options.courseHeight)),step=h/count;
        const sill=Math.max(0,Math.floor((ap.bottom-.1+EPS)/step)*step);
        if(sill>EPS)shells.push(prism(id+':below',[[a,0],[b,0],[b,sill],[a,sill]],point,t,stone));
        shells.push(prism(id+':sill',[[a,sill],[b,sill],[b,ap.bottom],[a,ap.bottom]],point,t+.1,stone,'sill'));
      }
    }
  }
  for(const r of manifest.junctions) {
    if(!r.ownedVolume)continue;
    const v=r.ownedVolume;
    // Site wings contain no curves, hence owned volumes need no tangent clip.
    shells.push(prism(r.id,[[v.minX,v.minY],[v.maxX,v.minY],[v.maxX,v.maxY],[v.minX,v.maxY]],
      (x,y,z)=>[x,y,(v.minZ+v.maxZ)/2+z],v.maxZ-v.minZ,stone,'junction'));
  }
  // Existing rail layout supplies physical dimensions and ownership. Bake its
  // matrices into vertices once, so runtime Parts need no scaling or children.
  for(const r of manifest.openBoundaries||[])for(const [i,b] of layoutOpenBoundary(r,manifest,options).placements.entries()) {
    const m=b.matrix,q=b.params;
    const point=(x,y,z)=>[m[0]*x+m[1]*y+m[2]*z+m[3],m[4]*x+m[5]*y+m[6]*z+m[7],m[8]*x+m[9]*y+m[10]*z+m[11]];
    shells.push(prism(r.id+':rail-'+i,[[-q.length/2,-q.height/2],[q.length/2,-q.height/2],[q.length/2,q.height/2],[-q.length/2,q.height/2]],point,q.width,options.oak,'rail'));
  }
  return {version:1,shells,sockets,census:{wallModules:manifest.wallModules.length,junctions:manifest.junctions.filter(r=>r.ownedVolume).length,
    openings:sockets.length,arches:sockets.filter(s=>s.arch).length,shells:shells.length,
    triangles:shells.reduce((n,s)=>n+s.faces.reduce((m,f)=>m+f.positions.length-2,0),0),children:0}};
}

export function emitSiteSurfaceMasonry(part,manifest,p={}) {
  const layout=layoutSiteSurfaceMasonry(manifest,p);
  const buckets=new Map();let active;
  const sink={fill(m){if(!buckets.has(m))buckets.set(m,[]);active=buckets.get(m);},beginShape(){},endShape(){},surfaceVertex(...v){active.push(v);}};
  for(const shell of layout.shells)emitSurfaceShell(sink,shell,{body:shell.material});
  for(const [material,vertices] of buckets){
    part.fill(material);part.beginShape(0);
    for(const vertex of vertices){
      if(typeof part.surfaceVertex==='function')part.surfaceVertex(...vertex);
      else part.vertex(...vertex.slice(0,3));
    }
    part.endShape();
  }
  return layout.census;
}
