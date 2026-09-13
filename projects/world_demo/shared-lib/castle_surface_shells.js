// Canonical structural shells, metres, +X timber length, +Y up, +Z width.
// No voxel sampling or simplification. These are untextured runtime envelopes;
// grain, wear and stone microdetail belong to the separate surface bake.
//
// Descriptor faces own an outward flat normal, material region and metric UV
// frame: position = origin + uAxis*u + vAxis*v. UV coordinates are metres,
// not a packed atlas. The current vertex(x,y,z) DSL transports positions only;
// surfaceVertex, when exposed by the host, transports normals and metric UVs;
// a legacy vertex-only host receives the matching planar geometric normals.

const frozen = value => {
  if (value && typeof value === 'object') {
    Object.values(value).forEach(frozen);
    Object.freeze(value);
  }
  return value;
};

export const CASTLE_SURFACE_SHELLS = frozen([
  { id: 'beam-1', kind: 'timber', size: [1, .28, .24], bevel: .006 },
  { id: 'beam-2', kind: 'timber', size: [2, .28, .24], bevel: .006 },
  { id: 'beam-4', kind: 'timber', size: [4, .28, .24], bevel: .006 },
  { id: 'plank-1', kind: 'timber', size: [1, .06, .28], bevel: .003 },
  { id: 'plank-2', kind: 'timber', size: [2, .06, .28], bevel: .003 },
  { id: 'floor-1', kind: 'stone', size: [1, .16, 1], bevel: .008 },
  { id: 'floor-2', kind: 'stone', size: [2, .16, 1], bevel: .008 },
  { id: 'floor-square-2', kind: 'stone', size: [2, .16, 2], bevel: .008 },
  { id: 'beam-4-strapped', kind: 'timber', size: [4, .28, .24], bevel: .006, straps: [-.75, .75] },
  ...[1,2,4].map(length=>({id:`rafter-${length}`,kind:'timber',size:[length,.18,.14],bevel:.004})),
  ...[1,2,4].map(length=>({id:`post-${length}`,kind:'timber',size:[length,.25,.25],bevel:.006})),
]);

// Stable Part.shape indices. Rafter(.14x.18) and post(.25x.25) are actual
// connector/structure families; varying bearer/joist sections are not silently
// substituted. Fifteen physical shell shapes total, including floors/hardware.
export const CASTLE_TIMBER_SURFACE_IDS = frozen(['beam-1','beam-2','beam-4','plank-1','plank-2',
  'beam-4-strapped','rafter-1','rafter-2','rafter-4','post-1','post-2','post-4']);

const add = (a, b) => a.map((v, i) => v + b[i]);
const sub = (a, b) => a.map((v, i) => v - b[i]);
const mul = (a, s) => a.map(v => v * s);
const dot = (a, b) => a.reduce((s, v, i) => s + v * b[i], 0);
const cross = (a, b) => [a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]];
const unit = a => mul(a, 1 / Math.hypot(...a));
const axis = (i, sign = 1) => [0, 1, 2].map(j => i === j ? sign : 0);

function vec3(value, label) {
  if (!Array.isArray(value) || value.length !== 3 || !value.every(Number.isFinite))
    throw new TypeError(`${label} must contain three finite numbers`);
  return [...value];
}

function face(points, normal, id, region) {
  const center = mul(points.reduce(add, [0, 0, 0]), 1 / points.length);
  // Timber longitudinal faces all use +X as grain U. End grain uses +Z.
  const uAxis = unit(Math.abs(normal[0]) > .999 ? [0, 0, 1]
    : sub([1, 0, 0], mul(normal, normal[0])));
  const vAxis = cross(normal, uAxis); // U x V = outward normal
  const ordered = [...points].sort((a, b) => {
    const pa = sub(a, center), pb = sub(b, center);
    return Math.atan2(dot(pa, vAxis), dot(pa, uAxis)) - Math.atan2(dot(pb, vAxis), dot(pb, uAxis));
  });
  return {
    id, region, normal, frame: { origin: center, uAxis, vAxis },
    positions: ordered,
    uv: ordered.map(p => [dot(sub(p, center), uAxis), dot(sub(p, center), vAxis)]),
  };
}

// Shared convex planar polygon leaf for structural wall/portal builders.
// Callers triangulate concave footprints before handing their cap faces here.
export function makeSurfaceFace(points, normal, id, region = 'body') {
  if (!Array.isArray(points) || points.length < 3) throw new TypeError('surface face requires at least three points');
  const p=points.map(v=>vec3(v,'surface face point')),n=vec3(normal,'surface normal');
  if(!(Math.hypot(...n)>0))throw new TypeError('surface normal must be nonzero');
  return face(p,unit(n),id,region);
}

// Closed metal strap sleeves, separate material/component ownership. The
// inner ring contacts the timber; the visible outer skin is 3mm proud. This
// is purposeful joinery hardware, not fibre geometry. Each sleeve is a closed
// solid and can be omitted by choosing the ordinary beam-4 catalogue entry.
function strapFaces(center, h, bevel, id) {
  const profile = (y, z) => [[y,z-bevel],[y-bevel,z],[-y+bevel,z],[-y,z-bevel],
    [-y,-z+bevel],[-y+bevel,-z],[y-bevel,-z],[y,-z+bevel]];
  const inner = profile(h[1], h[2]), outer = profile(h[1]+.003, h[2]+.003);
  const vertex = (ring, end, i) => [center + end*.02, ...ring[i%8]];
  const result = [];
  const quad = (p, outward, name) => {
    let n = unit(cross(sub(p[1],p[0]),sub(p[2],p[0])));
    if (dot(n,outward)<0) n=mul(n,-1);
    result.push({ ...face(p,n,`${id}-${name}`,'metal'), component: id });
  };
  for(let i=0;i<8;i++) {
    const radial=[0,outer[i][0]+outer[(i+1)%8][0],outer[i][1]+outer[(i+1)%8][1]];
    quad([vertex(outer,-1,i),vertex(outer,1,i),vertex(outer,1,i+1),vertex(outer,-1,i+1)],radial,`outer-${i}`);
    quad([vertex(inner,-1,i),vertex(inner,1,i),vertex(inner,1,i+1),vertex(inner,-1,i+1)],mul(radial,-1),`inner-${i}`);
    for(const end of [-1,1])
      quad([vertex(inner,end,i),vertex(inner,end,i+1),vertex(outer,end,i+1),vertex(outer,end,i)], [end,0,0],`end-${end}-${i}`);
  }
  return result;
}

// Pure immutable descriptor. The finite catalogue is geometry identity;
// palette handles and placements are supplied separately, never baked into it.
export function buildSurfaceShell(id = 'beam-2') {
  const spec = CASTLE_SURFACE_SHELLS.find(item => item.id === id);
  if (!spec) throw new RangeError(`unknown castle surface shell: ${id}`);
  return descriptor(spec, id);
}

// A residual is explicit cut geometry from one physical 1m timber blank. It
// does not add a recipe to the stock catalogue or stretch the source mesh.
export function buildCutTimberShell(length, family = 'beam') {
  if (!Number.isFinite(length) || length < 1e-5 || length >= 1)
    throw new RangeError('cut timber length must be >=0.00001 and <1 metre');
  if (!['beam','rafter','post'].includes(family))throw new RangeError('unknown physical timber family');
  const stockId=`${family}-1`;
  const spec = CASTLE_SURFACE_SHELLS.find(item=>item.id===stockId);
  return descriptor({ ...spec, size: [length, spec.size[1], spec.size[2]],
    bevel: Math.min(spec.bevel, length/4) }, `cut-${stockId}:${length}`,
    { stockId, length, removedLength: 1-length, cutEnd: '+X', sourceOffset: [-(1-length)/2,0,0] });
}

function descriptor(spec, id, cut = null) {
  const h = mul(spec.size, .5), b = spec.bevel;
  const vertices = [];
  for (const sx of [-1, 1]) for (const sy of [-1, 1]) for (const sz of [-1, 1]) {
    const signs = [sx, sy, sz];
    for (let a = 0; a < 3; a++) vertices.push(h.map((v, i) => signs[i]*(v - (i === a ? 0 : b))));
  }
  const faces = [];
  const plane = (normal, distance, name, region = 'body') => {
    const n = unit(normal);
    const points = vertices.filter(v => Math.abs(dot(v, normal) - distance) < 1e-10);
    if (points.length < 3) throw new Error(`invalid shell plane ${name}`);
    faces.push(face(points, n, name, region));
  };
  // Six structural faces, twelve bevel strips, eight clipped corners. Vertex
  // normals split per plane; no smoothing across the deliberately hard seams.
  for (let a = 0; a < 3; a++) for (const sign of [-1, 1])
    plane(axis(a, sign), h[a], `face-${a}-${sign}`,
      spec.kind === 'timber' && a === 0 ? 'endGrain' : 'body');
  for (let a = 0; a < 3; a++) for (let c = a+1; c < 3; c++)
    for (const sa of [-1, 1]) for (const sc of [-1, 1])
      plane(add(axis(a, sa), axis(c, sc)), h[a]+h[c]-b, `bevel-${a}-${c}-${sa}-${sc}`);
  for (const sx of [-1, 1]) for (const sy of [-1, 1]) for (const sz of [-1, 1])
    plane([sx, sy, sz], h[0]+h[1]+h[2]-2*b, `corner-${sx}-${sy}-${sz}`);
  for(const [index, center] of (spec.straps ?? []).entries())
    faces.push(...strapFaces(center,h,b,`strap-${index}`));
  const boundH=[...h];
  if(spec.straps) { boundH[1]+=.003; boundH[2]+=.003; }
  return frozen({
    version: 1, id, cut, kind: spec.kind, size: mul(boundH, 2), bodySize: [...spec.size], bevel: b,
    bounds: { min: mul(boundH, -1), max: [...boundH] },
    // Useful assembly contact frames; origin is the physical end/top plane.
    sockets: spec.kind === 'timber' ? [
      { id: 'start', origin: [-h[0], 0, 0], normal: [-1, 0, 0], width: spec.size[2], height: spec.size[1] },
      { id: 'end', origin: [h[0], 0, 0], normal: [1, 0, 0], width: spec.size[2], height: spec.size[1] },
    ] : [{ id: 'walk', origin: [0, h[1], 0], normal: [0, 1, 0], width: spec.size[0], depth: spec.size[2] }],
    faces,
  });
}

// Validate an explicit proper rigid frame. A 3x3 row-major rotation supports
// sloping braces/rafters; yaw is convenient sugar. No fit/scale is accepted.
export function makeSurfaceFrame(placement = {}) {
  for (const key of Object.keys(placement))
    if (key !== 'origin' && key !== 'yawDeg' && key !== 'rotation')
      throw new TypeError(`rigid shell placement does not accept ${key}`);
  const origin = vec3(placement.origin ?? [0, 0, 0], 'origin');
  let rotation;
  if (placement.rotation !== undefined) {
    if (placement.yawDeg !== undefined) throw new TypeError('specify rotation or yawDeg, not both');
    rotation = placement.rotation;
    if (!Array.isArray(rotation) || rotation.length !== 9 || !rotation.every(Number.isFinite))
      throw new TypeError('rotation must contain nine finite numbers');
    rotation = [...rotation];
  } else {
    const yawDeg = placement.yawDeg ?? 0;
    if (!Number.isFinite(yawDeg)) throw new TypeError('yawDeg must be finite');
    const c = Math.cos(yawDeg*Math.PI/180), s = Math.sin(yawDeg*Math.PI/180);
    rotation = [c,0,s,0,1,0,-s,0,c];
  }
  const rows = [rotation.slice(0,3),rotation.slice(3,6),rotation.slice(6,9)];
  for(let a=0;a<3;a++) for(let b=0;b<3;b++)
    if(Math.abs(dot(rows[a],rows[b])-(a===b?1:0))>1e-6)
      throw new TypeError('rotation must be orthonormal: no scale or shear');
  if(Math.abs(dot(rows[0],cross(rows[1],rows[2]))-1)>1e-6)
    throw new TypeError('rotation must preserve orientation: no reflection');
  return frozen({origin,rotation});
}

export function transformSurfacePoint(frame, point) {
  const r=frame.rotation;
  return frame.origin.map((v,a)=>v+dot(r.slice(a*3,a*3+3),point));
}

// Exact rigid placement preserves metre UV density. Dimension variants come
// from the bounded physical catalogue; there is no normalization or scale.
export function placeSurfaceShell(shell, placement = {}) {
  const frame = makeSurfaceFrame(placement);
  const rotate = v => [0,1,2].map(a=>dot(frame.rotation.slice(a*3,a*3+3),v));
  const point = v => transformSurfacePoint(frame,v);
  const faces = shell.faces.map(f => ({ ...f, normal: rotate(f.normal), ...(f.shadingNormals ? {shadingNormals:f.shadingNormals.map(rotate)} : {}), positions: f.positions.map(point),
    frame: { origin: point(f.frame.origin), uAxis: rotate(f.frame.uAxis), vAxis: rotate(f.frame.vAxis) } }));
  const points = faces.flatMap(f => f.positions);
  return frozen({ ...shell, placement: frame, faces,
    bounds: { min: [0,1,2].map(a => Math.min(...points.map(p => p[a]))),
      max: [0,1,2].map(a => Math.max(...points.map(p => p[a]))) },
    sockets: shell.sockets.map(socket => ({ ...socket, origin: point(socket.origin), normal: rotate(socket.normal) })),
    ...(shell.collision ? {collision:shell.collision.map(solid=>({kind:'convexHull',id:solid.id,
      vertices:solid.vertices ? solid.vertices.map(point) : [solid.bottom,solid.top].flatMap(y=>solid.footprint.map(p=>point([p[0],y,p[1]])))}))} : {}),
  });
}

// CPU direct-triangle emission using only APIs actually exposed by Part.
// Separate material buckets keep timber end grain distinct from longitudinal
// wood. No voxel, modifier, sphere, line, or fibre-overlay calls are made.
export function emitSurfaceShell(part, shell, materials) {
  for (const f of shell.faces) {
    const material = f.region === 'metal' ? materials.metal : (materials[f.region] ?? materials.body);
    if (!Number.isInteger(material) || material < 0) throw new TypeError(`missing material region ${f.region}`);
    part.fill(material);
    part.beginShape(0);
    for (let i = 1; i+1 < f.positions.length; i++)
      for (const corner of [0, i, i+1]) {
        if (typeof part.surfaceVertex === 'function')
          part.surfaceVertex(...f.positions[corner], ...(f.shadingNormals?.[corner] ?? f.normal), ...(f.sourceUV?.[corner] ?? f.uv[corner]));
        else part.vertex(...f.positions[corner]);
      }
    part.endShape();
  }
}
