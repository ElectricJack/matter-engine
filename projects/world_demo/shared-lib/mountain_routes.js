// Shared authored coordinates for roads, terrain grading and ecological
// clearance. All distances are physical world metres. No tile, LOD or camera
// enters compilation; consumers query the same immutable layout.
const EPS = 1e-8;
const clamp = x => Math.max(0, Math.min(1, x));
const smooth = x => { const t = clamp(x); return t*t*(3-2*t); };
const fail = message => { throw new Error(`mountain routes: ${message}`); };
function finite(value, name, lo = -Infinity, hi = Infinity) {
  if (!Number.isFinite(value) || value < lo || value > hi) fail(`invalid ${name}`);
  return value;
}
function identifier(value) {
  if (typeof value !== 'string' || !/^[A-Za-z0-9_-]{1,64}$/.test(value)) fail('invalid id');
  return value;
}
function freeze(value) {
  if (value && typeof value === 'object') {
    for (const child of Object.values(value)) freeze(child);
    Object.freeze(value);
  }
  return value;
}
const cross = (ax, az, bx, bz) => ax*bz-az*bx;
const boxOf = points => ({
  minX: Math.min(...points.map(p => p[0])), maxX: Math.max(...points.map(p => p[0])),
  minZ: Math.min(...points.map(p => p[2])), maxZ: Math.max(...points.map(p => p[2])),
});
function boundsCheck(b) {
  for (const key of ['minX','maxX','minZ','maxZ']) finite(b[key], key, -1e9, 1e9);
  if (b.maxX <= b.minX || b.maxZ <= b.minZ) fail('empty query bounds');
}

// Bounded compile-time and query broad phase. A long view query walks occupied
// records instead of iterating millions of empty cells. The buckets stay private.
function spatialIndex(records, cellSize) {
  const cells = new Map();
  let entries = 0;
  records.forEach((record, index) => {
    const b = record.bounds;
    const x0 = Math.floor(b.minX/cellSize), x1 = Math.floor(b.maxX/cellSize);
    const z0 = Math.floor(b.minZ/cellSize), z1 = Math.floor(b.maxZ/cellSize);
    const count = (x1-x0+1)*(z1-z0+1);
    if ((entries += count) > 262144) fail('spatial index budget exceeded');
    for (let z=z0; z<=z1; ++z) for (let x=x0; x<=x1; ++x) {
      const key = `${x},${z}`;
      if (!cells.has(key)) cells.set(key, []);
      cells.get(key).push(index);
    }
  });
  return b => {
    const x0 = Math.floor(b.minX/cellSize), x1 = Math.floor(b.maxX/cellSize);
    const z0 = Math.floor(b.minZ/cellSize), z1 = Math.floor(b.maxZ/cellSize);
    const count = (x1-x0+1)*(z1-z0+1);
    let ids;
    if (count > Math.max(64, cells.size)) ids = records.map((_, i) => i);
    else {
      const found = new Set();
      for (let z=z0; z<=z1; ++z) for (let x=x0; x<=x1; ++x)
        for (const i of cells.get(`${x},${z}`) || []) found.add(i);
      ids = [...found].sort((a,b) => a-b);
    }
    return ids.map(i => records[i]).filter(r => r.bounds.minX<=b.maxX &&
      r.bounds.maxX>=b.minX && r.bounds.minZ<=b.maxZ && r.bounds.maxZ>=b.minZ);
  };
}

function compileRoute(spec, maxStep) {
  const id = identifier(spec.id);
  const width = finite(spec.width ?? 6, 'road width', 1, 32);
  const shoulder = finite(spec.shoulder ?? 4, 'shoulder', .1, 32);
  const crossfall = finite(spec.crossfall ?? .02, 'crossfall', 0, .08);
  const maxGrade = finite(spec.maxGrade ?? .12, 'maximum grade', .001, .3);
  const maxMiter = finite(spec.maxMiter ?? 2, 'miter limit', 1, 4);
  if (!Array.isArray(spec.points) || spec.points.length<2 || spec.points.length>512)
    fail(`${id}: expected 2..512 route points`);
  const points = spec.points.map(p => {
    if (!Array.isArray(p) || p.length!==3) fail(`${id}: expected [x,y,z] point`);
    return p.map(v => finite(v, 'route coordinate', -1e6, 1e6));
  });
  const stations = [0];
  for (let i=1; i<points.length; ++i) {
    const a=points[i-1], b=points[i], distance=Math.hypot(b[0]-a[0],b[2]-a[2]);
    if (distance<.1) fail(`${id}: duplicate or vertical route segment`);
    if (Math.abs(b[1]-a[1])>maxGrade*distance+EPS) fail(`${id}: road exceeds maximum grade`);
    stations.push(stations[i-1]+distance);
  }
  const length = stations[stations.length-1];
  if (spec.tunnels !== undefined && (!Array.isArray(spec.tunnels) || spec.tunnels.length>128))
    fail(`${id}: tunnel budget exceeded`);
  const tunnels = (spec.tunnels || []).map(t => ({
    id: `${id}/${t.id === undefined ? `tunnel-${t.start}-${t.end}` : identifier(t.id)}`,
    start: finite(t.start,'tunnel start',0,length),
    end: finite(t.end,'tunnel end',0,length),
    width: finite(t.width ?? width+2,'tunnel clear width',width,64),
    height: finite(t.height ?? 6,'tunnel clear height',3,32),
    portalBlend: finite(t.portalBlend ?? 12,'portal blend',1,64),
  })).sort((a,b) => a.start-b.start);
  for (let i=0; i<tunnels.length; ++i) {
    const t=tunnels[i];
    if (t.end-t.start<4 || t.start<t.portalBlend || length-t.end<t.portalBlend)
      fail(`${id}: tunnel needs an interior and two approach lengths`);
    if (i && tunnels[i-1].end+tunnels[i-1].portalBlend>=t.start-t.portalBlend)
      fail(`${id}: overlapping tunnel approaches`);
    if (tunnels.slice(0,i).some(other => other.id===t.id)) fail(`${id}: duplicate tunnel id`);
  }
  const rows=[];
  for (let i=0; i<points.length-1; ++i) {
    const a=points[i], b=points[i+1], run=stations[i+1]-stations[i];
    const count=Math.ceil(run/maxStep);
    if (rows.length+count+1>8192) fail(`${id}: route sample budget exceeded`);
    for (let j=0; j<count; ++j) {
      const t=j/count;
      rows.push({position:a.map((v,k) => v+(b[k]-v)*t), station:stations[i]+run*t});
    }
  }
  rows.push({position:points[points.length-1], station:length});
  // Bisector miters share EXACT row positions at bends. Reject folds rather
  // than clamping each segment independently and opening cracks at the join.
  for (let i=0; i<rows.length; ++i) {
    const a=rows[Math.max(0,i-1)].position, b=rows[i].position;
    const c=rows[Math.min(rows.length-1,i+1)].position;
    const direction=(p,q) => { const l=Math.hypot(q[0]-p[0],q[2]-p[2]);
      return [(q[0]-p[0])/l,(q[2]-p[2])/l]; };
    const before=i ? direction(a,b) : direction(b,c);
    const after=i+1<rows.length ? direction(b,c) : before;
    const d=1+before[0]*after[0]+before[1]*after[1];
    if (d<EPS) fail(`${id}: reversed route needs a rounded bend`);
    const miter=[-(before[1]+after[1])/d,(before[0]+after[0])/d];
    if (Math.hypot(...miter)>maxMiter+EPS) fail(`${id}: bend exceeds miter limit`);
    rows[i].miter=miter;
    rows[i].tangent=[miter[1]/Math.hypot(...miter),-miter[0]/Math.hypot(...miter)];
    rows[i].vertices=[-width/2-shoulder,-width/2,0,width/2,width/2+shoulder].map(offset =>
      [b[0]+miter[0]*offset,b[1]-crossfall*Math.min(Math.abs(offset),width/2),b[2]+miter[1]*offset]);
  }
  // Blend beyond the road ends, not under the actual road surface. Cap rows
  // are grading/shoulder records, so a road renderer can omit them.
  const cap=(row,sign) => {
    const delta=[sign*shoulder*row.tangent[0],0,sign*shoulder*row.tangent[1]];
    return {...row,station:row.station+sign*shoulder,cap:true,
      position:row.position.map((v,k) => v+delta[k]),
      vertices:row.vertices.map(v => v.map((a,k) => a+delta[k]))};
  };
  const first=cap(rows[0],-1),last=cap(rows[rows.length-1],1);
  rows.unshift(first); rows.push(last);
  return {id,width,shoulder,crossfall,maxGrade,length,tunnels,rows};
}

function sectionAt(route, station) {
  finite(station,'station',0,route.length);
  let lo=0, hi=route.rows.length-1;
  while (lo+1<hi) { const mid=(lo+hi)>>1;
    if (route.rows[mid].station<=station) lo=mid; else hi=mid;
  }
  const a=route.rows[lo], b=route.rows[hi];
  const t=(station-a.station)/(b.station-a.station);
  const position=a.position.map((v,k) => v+(b.position[k]-v)*t);
  const miter=a.miter.map((v,k) => v+(b.miter[k]-v)*t), ml=Math.hypot(...miter);
  return {station,position,normal:miter.map(v => v/ml),tangent:[miter[1]/ml,-miter[0]/ml]};
}

function gradingAt(route, station) {
  // Rounded axial fade at the authored ends, avoiding abrupt cut/fill walls.
  let w=smooth(1+station/route.shoulder)*smooth(1+(route.length-station)/route.shoulder);
  for (const t of route.tunnels) {
    if (station>t.start && station<t.end)
      w*=Math.max(smooth(1-(station-t.start)/t.portalBlend),smooth(1-(t.end-station)/t.portalBlend));
  }
  return w;
}

function barycentric(tri,x,z) {
  const [a,b,c]=tri.vertices;
  const area=cross(b[0]-a[0],b[2]-a[2],c[0]-a[0],c[2]-a[2]);
  const v=cross(x-a[0],z-a[2],c[0]-a[0],c[2]-a[2])/area;
  const w=cross(b[0]-a[0],b[2]-a[2],x-a[0],z-a[2])/area;
  return [1-v-w,v,w];
}

// Strict positive-area intersection, with touching edges allowed. A crossing
// requires an explicitly authored junction/bridge; silently averaging heights
// would make rendering, collision and the terrain grade disagree.
function overlaps(a,b) {
  for (const polygon of [a,b]) for (let i=0; i<polygon.length; ++i) {
    const p=polygon[i],q=polygon[(i+1)%polygon.length],nx=-(q[2]-p[2]),nz=q[0]-p[0];
    const project=poly => poly.map(v => v[0]*nx+v[2]*nz);
    const aa=project(a),bb=project(b);
    if (Math.max(...aa)<=Math.min(...bb)+EPS || Math.max(...bb)<=Math.min(...aa)+EPS) return false;
  }
  return true;
}

/** Compile explicit world-space route elevations once, before sector queries.
 * points are crown elevations; callers must fit/probe them against unmodified
 * terrain. This module does not invent a terrain sample during field compilation.
 * Sites are oriented pads beside a road; their dimensions describe a footprint,
 * not a house mesh. Tunnel records reserve real excavation/interior work.
 */
export function compileMountainRoutes({routes,sites=[],maxStep=8,cellSize=64}) {
  finite(maxStep,'sample step',1,32); finite(cellSize,'index cell size',16,256);
  if (!Array.isArray(routes) || routes.length>64 || !Array.isArray(sites) || sites.length>512)
    fail('layout budget exceeded');
  let rowCount=0;
  const roads=routes.map(r => {
    const road=compileRoute(r,maxStep);
    if ((rowCount+=road.rows.length)>16384) fail('layout section budget exceeded');
    return road;
  }).sort((a,b) => a.id<b.id?-1:a.id>b.id?1:0);
  const byId=new Map();
  for (const r of roads) { if (byId.has(r.id)) fail('duplicate route id'); byId.set(r.id,r); }
  const sections=[], triangles=[];
  for (const road of roads) for (let i=0; i<road.rows.length-1; ++i) {
    const a=road.rows[i],b=road.rows[i+1];
    const polygon=[a.vertices[0],a.vertices[4],b.vertices[4],b.vertices[0]];
    // A ribbon may fold even below the angular miter limit when successive
    // corners are too close for its full shoulder width.
    for (let j=0;j<4;++j) {
      const p=polygon[j],q=polygon[(j+1)%4],r=polygon[(j+2)%4];
      if (cross(q[0]-p[0],q[2]-p[2],r[0]-q[0],r[2]-q[2])>=-EPS)
        fail(`${road.id}: folded road ribbon; widen the bend`);
    }
    const section={id:`${road.id}/section/${i}`,route:road.id,index:i,cap:!!(a.cap||b.cap),
      station0:a.station,station1:b.station,polygon,bounds:boxOf(polygon),
      ownerX:(a.position[0]+b.position[0])/2,ownerZ:(a.position[2]+b.position[2])/2};
    sections.push(section);
    for (let band=0;band<4;++band) {
      const verts=[a.vertices[band],a.vertices[band+1],b.vertices[band+1],b.vertices[band]];
      const lateral=[band===0?0:1,band===3?0:1,band===3?0:1,band===0?0:1];
      for (const indices of [[0,1,2],[0,2,3]]) {
        const vertices=indices.map(j => verts[j]);
        triangles.push({id:`${section.id}/${band}/${indices[2]}`,section:section.id,route:road.id,
          band:section.cap||band===0||band===3?'shoulder':'road',vertices,bounds:boxOf(vertices),
          lateral:indices.map(j => lateral[j]),stations:indices.map(j => j<2?a.station:b.station)});
      }
    }
  }
  if (sections.length>16384) fail('layout section budget exceeded');
  const querySections=spatialIndex(sections,cellSize);
  for (const section of sections) for (const other of querySections(section.bounds)) {
    if (other.id<=section.id || other.route===section.route && Math.abs(other.index-section.index)<=1) continue;
    if (overlaps(section.polygon,other.polygon)) fail('overlapping roads need an authored junction or bridge');
  }
  const queryTriangles=spatialIndex(triangles,cellSize);
  const usedSites=new Set();
  const pads=sites.map(s => {
    const id=identifier(s.id),road=byId.get(s.route);
    if (usedSites.has(id)) fail('duplicate site id'); usedSites.add(id);
    if (!road) fail(`${id}: missing site route`);
    const station=finite(s.station,'site station',0,road.length),frame=sectionAt(road,station);
    const side=s.side??1; if (side!==1 && side!==-1) fail('invalid site side');
    const width=finite(s.width??10,'site width',2,64),depth=finite(s.depth??12,'site depth',2,64);
    const setback=finite(s.setback??3,'site setback',0,128),blend=finite(s.blend??4,'site blend',.1,32);
    if (road.tunnels.some(t => station+width/2>=t.start-t.portalBlend && station-width/2<=t.end+t.portalBlend))
      fail(`${id}: site overlaps a tunnel approach`);
    const offset=side*(road.width/2+road.shoulder+setback+depth/2);
    const position=[frame.position[0]+offset*frame.normal[0],
      finite(s.elevation??frame.position[1],'site elevation',-1e6,1e6),frame.position[2]+offset*frame.normal[1]];
    return {id,route:road.id,station,side,position,tangent:frame.tangent,normal:frame.normal,width,depth,blend};
  }).sort((a,b) => a.id<b.id?-1:a.id>b.id?1:0);
  const portals=roads.flatMap(road => road.tunnels.flatMap(t => [t.start,t.end].map((station,i) => ({
    id:`${t.id}/${i?'exit':'entry'}`,route:road.id,tunnel:t.id,end:i?'exit':'entry',
    ...sectionAt(road,station),width:t.width,height:t.height,blend:t.portalBlend,
  }))));
  freeze(roads); freeze(sections); freeze(triangles); freeze(pads); freeze(portals);
  return Object.freeze({roads,sections,triangles,sites:pads,portals,
    at(route,station) { const road=byId.get(route); if (!road) fail('unknown route'); return sectionAt(road,station); },
    sectionsIn(bounds) { boundsCheck(bounds); return querySections(bounds); },
    // Single ownership for rendering/instances. Grading instead queries overlap:
    // clipping to a sector is NEVER part of the material or terrain definition.
    ownedSections(bounds) { boundsCheck(bounds); return querySections(bounds).filter(s =>
      s.ownerX>=bounds.minX && s.ownerX<bounds.maxX && s.ownerZ>=bounds.minZ && s.ownerZ<bounds.maxZ); },
    gradeAt(x,z,baseHeight) {
      finite(x,'x',-1e9,1e9); finite(z,'z',-1e9,1e9); finite(baseHeight,'base height');
      let result={height:baseHeight,weight:0,road:null,station:null};
      for (const tri of queryTriangles({minX:x,maxX:x,minZ:z,maxZ:z})) {
        const b=barycentric(tri,x,z); if (b.some(v => v < -EPS)) continue;
        const interpolate=values => b.reduce((sum,v,i) => sum+v*values[i],0);
        const station=interpolate(tri.stations),road=byId.get(tri.route);
        const weight=smooth(interpolate(tri.lateral))*gradingAt(road,station);
        if (weight<=result.weight) continue;
        const target=interpolate(tri.vertices.map(v => v[1]));
        result={height:baseHeight+(target-baseHeight)*weight,weight,road:road.id,station};
      }
      return result;
    },
    available(x,z,radius=0) {
      finite(x,'x',-1e9,1e9); finite(z,'z',-1e9,1e9); finite(radius,'clearance radius',0,1024);
      for (const section of querySections({minX:x-radius,maxX:x+radius,minZ:z-radius,maxZ:z+radius})) {
        const road=byId.get(section.route),a=road.rows[section.index].position,b=road.rows[section.index+1].position;
        const dx=b[0]-a[0],dz=b[2]-a[2],t=clamp(((x-a[0])*dx+(z-a[2])*dz)/(dx*dx+dz*dz));
        const station=section.station0+(section.station1-section.station0)*t;
        if (road.tunnels.some(t => station>t.start+t.portalBlend+radius && station<t.end-t.portalBlend-radius)) continue;
        if (Math.hypot(x-a[0]-t*dx,z-a[2]-t*dz)<=road.width/2+road.shoulder+radius) return false;
        // At a bend the miter extends beyond the centreline's round envelope.
        const p=section.polygon;
        if (p.every((v,i) => { const q=p[(i+1)%p.length];
          return cross(q[0]-v[0],q[2]-v[2],x-v[0],z-v[2])<=radius*Math.hypot(q[0]-v[0],q[2]-v[2])+EPS;
        })) return false;
      }
      for (const s of pads) {
        const dx=x-s.position[0],dz=z-s.position[2];
        const along=Math.abs(dx*s.tangent[0]+dz*s.tangent[1])-s.width/2;
        const across=Math.abs(dx*s.normal[0]+dz*s.normal[1])-s.depth/2;
        if (Math.hypot(Math.max(0,along),Math.max(0,across))<=s.blend+radius) return false;
      }
      return true;
    },
  });
}
