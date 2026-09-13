// Closed structural wall envelopes from known dimensions and opening profiles.
// Metres, baseY=0, centered along local+X, positive thickness towards+Z.
// Collision cells and surface geometry come from the same solid occupancy;
// openings are never covered by an enclosing collision AABB.
//
// Integration contract (production migration is intentionally separate):
// - Feed compiled wall runs AFTER trim/junction ownership, not raw overlapping
//   edges. The ten templates are fixtures/reusable parts, not a replacement
//   floorplan compiler. Miter fixtures own one corner's complete footprint.
// - Openings are rectangular intervals in wall U and height. Arch profiles
//   require a profile-aware builder and are explicitly rejected, never boxed.
// - Portal bottoms remain open at floor level; sill/head/reveal surfaces only
//   bound actual solid tiles. Collision adapters consume these same prisms.
// - Arc U is centerline arc length. clearWidth reports the smaller inner-mouth
//   chord width; door/glazing fits must use that clearance, not the arc length.
// - Keep stairwell/floor holes, thresholds and connector promised headroom in
//   the authoritative structure/collision plan. A cosmetic shell cannot close
//   them or move rafters into their clearance envelopes.
// - Material/UV regions are geometry correspondence only. Surface baking,
//   glass/door placement and support joints remain separate owned products.
import { makeSurfaceFace, emitSurfaceShell } from 'shared-lib/castle_surface_shells';

export const CASTLE_WALL_SURFACES = Object.freeze([
  {id:'solid-1',length:1}, {id:'solid-2',length:2}, {id:'solid-4',length:4},
  {id:'door-2',length:2,openings:[{id:'door',start:.5,end:1.5,bottom:0,top:2.2}]},
  {id:'window-2',length:2,openings:[{id:'window',start:.5,end:1.5,bottom:1,top:2.2}]},
  {id:'join-15',joinDeg:15}, {id:'join-30',joinDeg:30}, {id:'join-45',joinDeg:45},
  {id:'arc-30',radius:4,angleDeg:30},
  {id:'arc-window-45',radius:4,angleDeg:45,openings:[{id:'window',start:1,end:2,bottom:1,top:2.2}]},
].map(spec=>Object.freeze(spec)));
const cross=(a,b)=>[a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]];
const sub=(a,b)=>a.map((v,i)=>v-b[i]);
const unit=v=>v.map(x=>x/Math.hypot(...v));
const sum=(a,b)=>a.map((v,i)=>v+b[i]);
function positive(v,name){if(!Number.isFinite(v)||v<=0)throw new RangeError(`${name} must be positive and finite`);return v;}
const unique=values=>[...new Set(values)].sort((a,b)=>a-b);
function finish(spec,faces,collision,sockets,parameterization){
  const points=faces.flatMap(f=>f.positions);
  if(!points.length)points.push([0,0,0]);
  return {version:1,id:spec.id??'dimensioned-wall',kind:'wall',faces,collision,sockets,parameterization,
    size:[0,1,2].map(a=>Math.max(...points.map(p=>p[a]))-Math.min(...points.map(p=>p[a]))),
    bounds:{min:[0,1,2].map(a=>Math.min(...points.map(p=>p[a]))),max:[0,1,2].map(a=>Math.max(...points.map(p=>p[a])))} };
}

// Explicit dimensioned descriptor seam for a compiled floorplan. It creates
// direct source geometry, not an unbounded set of registered stock Part keys.
export function wallSurfaceDescriptor(input) {
  const spec={height:3,thickness:.42,chordError:.002,...input};
  const height=positive(spec.height,'height'),thickness=positive(spec.thickness,'thickness');
  if(spec.joinDeg!==undefined){
    if(spec.openings?.length)throw new RangeError('miter opening profiles require explicit clipped plan geometry');
    return joinSurface(spec);
  }
  const curved=spec.radius!==undefined;
  const radius=curved?positive(spec.radius,'radius'):0;
  if(curved&&radius<=thickness/2)throw new RangeError('arc inner radius must be positive');
  const angle=curved?positive(spec.angleDeg,'angleDeg')*Math.PI/180:0;
  if(angle>Math.PI)throw new RangeError('split arcs larger than180degrees into sections');
  const length=curved?radius*angle:positive(spec.length,'length');
  const openings=(spec.openings??[]).map((a,index)=>{
    if(a.kind!==undefined&&!['door','window','rect'].includes(a.kind))throw new RangeError('only rectangular wall opening profiles are supported');
    if(![a.start,a.end,a.bottom,a.top].every(Number.isFinite)||a.start<0||a.end>length||a.start>=a.end||a.bottom<0||a.top>height||a.bottom>=a.top)
      throw new RangeError(`invalid wall opening ${index}`);
    const clearWidth=curved?2*(radius-thickness/2)*Math.sin((a.end-a.start)/(2*radius)):a.end-a.start;
    if(a.minClearWidth!==undefined&&(!Number.isFinite(a.minClearWidth)||a.minClearWidth<=0||clearWidth<a.minClearWidth))
      throw new RangeError('wall opening is narrower than promised minClearWidth');
    return {...a,id:a.id??`opening-${index}`,clearWidth};
  });
  for(let i=0;i<openings.length;i++)for(let j=0;j<i;j++){
    const a=openings[i],b=openings[j];
    if(Math.max(a.start,b.start)<Math.min(a.end,b.end)&&Math.max(a.bottom,b.bottom)<Math.min(a.top,b.top))
      throw new RangeError('overlapping wall openings must be unioned by the floorplan compiler');
  }
  const cuts=[0,length,...openings.flatMap(a=>[a.start,a.end])];
  if(curved){
    const error=positive(spec.chordError,'chordError'),outer=radius+thickness/2;
    const step=2*Math.acos(Math.max(-1,1-Math.min(error,outer)/outer));
    const n=Math.ceil(angle/step);
    if(n>1024)throw new RangeError('arc tessellation budget exceeded');
    for(let i=1;i<n;i++)cuts.push(length*i/n);
  }
  const us=unique(cuts),ys=unique([0,height,...openings.flatMap(a=>[a.bottom,a.top])]);
  if((us.length-1)*(ys.length-1)>65536)throw new RangeError('wall profile cell budget exceeded');
  const point=(u,y,w)=>curved?[(radius+w)*Math.sin(u/radius-angle/2),y,(radius+w)*Math.cos(u/radius-angle/2)-radius]:[u-length/2,y,w];
  const outward=u=>curved?[Math.sin(u/radius-angle/2),0,Math.cos(u/radius-angle/2)]:[0,0,1];
  const tangent=u=>curved?[Math.cos(u/radius-angle/2),0,-Math.sin(u/radius-angle/2)]:[1,0,0];
  const occupied=(i,j)=>{
    if(i<0||j<0||i>=us.length-1||j>=ys.length-1)return false;
    const u=(us[i]+us[i+1])/2,y=(ys[j]+ys[j+1])/2;
    return !openings.some(a=>u>a.start&&u<a.end&&y>a.bottom&&y<a.top);
  };
  const faces=[],collision=[],sockets=[];
  const add=(p,n,id,region='body',chart=null)=>{
    const f=makeSurfaceFace(p,n,id,region);
    if(chart){
      // Exact arc-length source coordinates accompany the local planar metre
      // frame. The current direct DSL transports geometric facet normals;
      // desired radial corner normals stay explicit for attributed transport.
      f.chart=chart;
      f.sourceUV=f.positions.map(p=>[(radius+chart.side*thickness/2)*(Math.atan2(p[0],p[2]+radius)+angle/2),p[1]]);
      f.shadingNormals=f.positions.map(p=>unit([p[0]*chart.side,0,(p[2]+radius)*chart.side]));
    }
    faces.push(f);
  };
  const half=thickness/2;
  for(let i=0;i<us.length-1;i++)for(let j=0;j<ys.length-1;j++)if(occupied(i,j)){
    const a=us[i],b=us[i+1],bottom=ys[j],top=ys[j+1],tag=`${i}-${j}`;
    for(const side of [-1,1])add([point(a,bottom,side*half),point(b,bottom,side*half),point(b,top,side*half),point(a,top,side*half)],outward((a+b)/2).map(v=>v*side),`skin-${side}-${tag}`,'body',curved?{kind:'arc',radius:radius+side*half,side}:null);
    for(const [neighbor,u,sign] of [[i-1,a,-1],[i+1,b,1]])if(!occupied(neighbor,j))
      add([point(u,bottom,-half),point(u,bottom,half),point(u,top,half),point(u,top,-half)],tangent(u).map(v=>v*sign),`end-${sign}-${tag}`,neighbor<0||neighbor>=us.length-1?'edge':'reveal');
    for(const [neighbor,y,sign] of [[j-1,bottom,-1],[j+1,top,1]])if(!occupied(i,neighbor))
      add([point(a,y,-half),point(b,y,-half),point(b,y,half),point(a,y,half)],[0,sign,0],`cap-${sign}-${tag}`,neighbor<0||neighbor>=ys.length-1?'edge':'reveal');
    // One convex prism per occupied profile tile. The exact same boundary
    // vertices define render and collision solids, including arc chord planes.
    collision.push({kind:'convexPrism',id:`solid-${tag}`,footprint:[point(a,0,-half),point(b,0,-half),point(b,0,half),point(a,0,half)].map(p=>[p[0],p[2]]),bottom,top});
  }
  for(const a of openings)sockets.push({id:a.id,kind:a.bottom===0?'door':'window',origin:point((a.start+a.end)/2,a.bottom,0),normal:outward((a.start+a.end)/2),width:a.end-a.start,clearWidth:a.clearWidth,height:a.top-a.bottom,
    void:{start:a.start,end:a.end,bottom:a.bottom,top:a.top},widthConvention:curved?'centerline-arc-length':'linear'});
  return finish(spec,faces,collision,sockets,{kind:curved?'arc':'planar',length,height,thickness,...(curved?{radius,angleDeg:spec.angleDeg,chordError:spec.chordError}:{})});
}

// Deterministic ear clipping of a simple CCW footprint; only concave miter
// caps need it. Side faces stay quads and keep shared physical corner points.
function triangulate(poly){
  const area=poly.reduce((s,p,i)=>{const q=poly[(i+1)%poly.length];return s+p[0]*q[1]-q[0]*p[1];},0);
  const ids=poly.map((_,i)=>i);if(area<0)ids.reverse();const out=[];
  const orient=(a,b,c)=>(b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]);
  while(ids.length>3){
    let found=false;
    for(let i=0;i<ids.length;i++){
      const a=ids[(i+ids.length-1)%ids.length],b=ids[i],c=ids[(i+1)%ids.length];
      if(orient(poly[a],poly[b],poly[c])<=1e-12)continue;
      if(ids.some(k=>k!==a&&k!==b&&k!==c&&orient(poly[a],poly[b],poly[k])>=-1e-12&&orient(poly[b],poly[c],poly[k])>=-1e-12&&orient(poly[c],poly[a],poly[k])>=-1e-12))continue;
      out.push([a,b,c]);ids.splice(i,1);found=true;break;
    }
    if(!found)throw new Error('wall footprint cannot be triangulated');
  }
  out.push([...ids]);return out;
}
function joinSurface(spec){
  const angle=spec.joinDeg*Math.PI/180;
  if(![15,30,45].includes(spec.joinDeg))throw new RangeError('joinDeg must be15,30or45');
  const half=spec.thickness/2,arm=spec.armLength??1;positive(arm,'armLength');
  const c=Math.cos(angle),s=Math.sin(angle),miter=[-Math.tan(angle/2)*half,half];
  const poly=[[-arm,-half],[-miter[0],-miter[1]],[arm*c+s*half,arm*s-c*half],
    [arm*c-s*half,arm*s+c*half],miter,[-arm,half]];
  const faces=[],at=(p,y)=>[p[0],y,p[1]],triangles=triangulate(poly);
  for(const [i,t] of triangles.entries())for(const [y,sign] of [[0,-1],[spec.height,1]])
    faces.push(makeSurfaceFace(t.map(k=>at(poly[k],y)),[0,sign,0],`join-cap-${sign}-${i}`,'edge'));
  for(let i=0;i<poly.length;i++){
    const a=poly[i],b=poly[(i+1)%poly.length],normal=[b[1]-a[1],0,a[0]-b[0]];
    faces.push(makeSurfaceFace([at(a,0),at(b,0),at(b,spec.height),at(a,spec.height)],normal,`join-side-${i}`));
  }
  const collision=triangles.map((t,i)=>({kind:'convexPrism',id:`join-solid-${i}`,footprint:t.map(k=>[...poly[k]]),bottom:0,top:spec.height}));
  return finish(spec,faces,collision,[{id:'start',origin:[-arm,0,0],normal:[-1,0,0],width:spec.thickness,height:spec.height},{id:'end',origin:[arm*c,0,arm*s],normal:[c,0,s],width:spec.thickness,height:spec.height}],{kind:'miter',joinDeg:spec.joinDeg,armLength:arm,height:spec.height,thickness:spec.thickness});
}
export function buildWallSurface(id='solid-1'){
  const spec=CASTLE_WALL_SURFACES.find(s=>s.id===id);if(!spec)throw new RangeError(`unknown wall surface ${id}`);
  return wallSurfaceDescriptor(spec);
}
export function emitWallSurface(part,shell,materials){emitSurfaceShell(part,shell,materials);}
