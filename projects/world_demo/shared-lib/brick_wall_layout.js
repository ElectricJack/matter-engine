// One physical layout for geometry, finite texture stamps and mortar. Metres.
// Anchor: lower left, near corner. Resizing preserves placements at this anchor.
export const MODULAR_CLAY_BRICK = Object.freeze({ lengthM: .245, heightM: .084, depthM: .1175 });
const EPS = 1e-9;
function positive(x, name) {
  if (!Number.isFinite(x) || x <= 0) throw new RangeError(`${name} must be positive metres`);
  return x;
}
function count(n, name) {
  if (!Number.isInteger(n) || n < 1 || n > 4096) throw new RangeError(`${name} must be an integer from 1 to 4096`);
  return n;
}
function fittedCount(n, requested, unit, joint, fit, name) {
  if (requested === undefined) return count(n ?? 4, name);
  if (n !== undefined) throw new RangeError(`choose ${name} or requested size, not both`);
  positive(requested, name + ' requested size');
  const cells = (requested + joint) / (unit + joint);
  const snap = Math.round(cells);
  const exact = Math.abs(cells - snap) < EPS ? snap : cells;
  const result = fit === 'inside' ? Math.floor(exact) : fit === 'outside' ? Math.ceil(exact) : Math.round(exact);
  if (fit === 'inside' && result < 1) throw new RangeError(`requested ${name} cannot contain one whole brick`);
  return count(Math.max(1, result), name);
}
function hash(seed, id) {
  let h = (2166136261 ^ seed) >>> 0;
  for (let i = 0; i < id.length; ++i) h = Math.imul(h ^ id.charCodeAt(i), 16777619) >>> 0;
  h = Math.imul(h ^ (h >>> 16), 0x7feb352d);
  h = Math.imul(h ^ (h >>> 15), 0x846ca68b);
  return (h ^ (h >>> 16)) >>> 0;
}
const wrapCell = (value, period) => ((value % period) + period) % period;

// This is the material's repeat domain, not a finite wall's exposed bounds.
// The terminal head/bed joints belong to the period. Wall extent, weathering
// and placement phase deliberately do not enter the reusable layout identity.
export function brickSurfaceModule(p = {}) {
  const bond = p.bond ?? 'running-headers';
  if (!['stack','running-headers'].includes(bond)) throw new RangeError('unknown module bond');
  const brick = { ...MODULAR_CLAY_BRICK, ...p.brick };
  for (const key of ['lengthM','heightM','depthM']) positive(brick[key],key);
  const head = positive(p.headJointM ?? .010,'head joint');
  const bed = positive(p.bedJointM ?? .010,'bed joint');
  if (bond === 'running-headers' && Math.abs(brick.lengthM - (2*brick.depthM+head)) > EPS)
    throw new RangeError('running module requires a modular header brick');
  const columns = count(p.moduleColumns ?? 8,'moduleColumns');
  const courses = count(p.moduleCourses ?? 4,'moduleCourses');
  if (bond === 'running-headers' && (courses & 1))
    throw new RangeError('running module needs an even number of courses');
  const seed = p.seed ?? 0;
  if (!Number.isInteger(seed) || seed < 0 || seed > 0xffffffff) throw new RangeError('seed must be uint32');
  const phase = [p.phaseColumns ?? 0,p.phaseCourses ?? 0];
  if (!phase.every(n => Number.isSafeInteger(n) && Math.abs(n) <= 0x7fffffff))
    throw new RangeError('module phase must use signed integer brick/course counts');
  if (bond === 'running-headers' && phase[1] % 2 !== 0)
    throw new RangeError('running module course phase must preserve bond parity');
  const definition = { version:1,bond,brick,headJointM:head,bedJointM:bed,columns,courses,seed };
  return { ...definition,
    periodM:[columns*(brick.lengthM+head),courses*(brick.heightM+bed)],
    phase:[wrapCell(phase[0],columns),wrapCell(phase[1],courses)],
    layoutKey:JSON.stringify(definition) };
}

// Stable addresses also work for ghost neighbours at negative module indices.
// End headers are explicit boundary treatments; they never become an interior
// half-brick just to make the repeat fit. Appearance phase leaves geometry fixed.
export function brickModuleAddress(module, course, column, wythe = 0, role = 'stretcher') {
  if (![course,column,wythe].every(Number.isSafeInteger) ||
      !['stretcher','header-left','header-right'].includes(role))
    throw new RangeError('invalid brick module address');
  const row = wrapCell(wrapCell(course,module.courses)+module.phase[1],module.courses);
  const col = wrapCell(wrapCell(column,module.columns)+module.phase[0],module.columns);
  const appearanceSeed = hash(module.seed,`m${row}/w${wythe}/${role}/${col}`);
  return { course:row,column:col,wythe,role,appearanceSeed,variant:appearanceSeed%8 };
}
// Six metric source frames; normals and UVs rotate with each actual brick.
// These are projection inputs, not an atlas or repeating wall texture.
function sourceFaces(l, h, d) {
  const face = (id, originM, u, v, n, widthM, heightM) => ({ id, originM, u, v, n, widthM, heightM });
  return [
    face('front', [0,h/2,d/2], [1,0,0], [0,1,0], [0,0,1], l,h),
    face('back', [0,h/2,-d/2], [-1,0,0], [0,1,0], [0,0,-1], l,h),
    face('right', [l/2,h/2,0], [0,0,-1], [0,1,0], [1,0,0], d,h),
    face('left', [-l/2,h/2,0], [0,0,1], [0,1,0], [-1,0,0], d,h),
    face('top', [0,h,0], [1,0,0], [0,0,-1], [0,1,0], l,d),
    face('bottom', [0,0,0], [1,0,0], [0,0,1], [0,-1,0], l,d),
  ];
}
export function brickWallLayout(p = {}) {
  if (!p || typeof p !== 'object' || Array.isArray(p)) throw new TypeError('wall options must be an object');
  const bond = p.bond ?? 'running-headers', fit = p.fit ?? 'nearest';
  if (!['stack', 'running-headers'].includes(bond)) throw new RangeError('use stack or running-headers bond');
  if (!['inside', 'outside', 'nearest'].includes(fit)) throw new RangeError('fit must be inside, outside or nearest');
  if (p.openings?.length || p.corner !== undefined)
    throw new RangeError('openings/corners require a whole-brick junction layout; use separate solid panels for now');
  const brick = { ...MODULAR_CLAY_BRICK, ...p.brick };
  const l = positive(brick.lengthM, 'brick length'), h = positive(brick.heightM, 'brick height');
  const d = positive(brick.depthM, 'brick depth');
  const head = positive(p.headJointM ?? .010, 'head joint'), bed = positive(p.bedJointM ?? .010, 'bed joint');
  if (bond === 'running-headers' && Math.abs(l - (2*d + head)) > EPS)
    throw new RangeError(`whole header ends require length = 2*depth + headJoint (${2*d+head} m); use a modular brick or stack bond`);
  const columns = fittedCount(p.columns, p.widthM, l, head, fit, 'columns');
  const courses = fittedCount(p.courses, p.heightM, h, bed, fit, 'courses');
  const thicknessM = bond === 'stack' ? d : l;
  if (p.thicknessM !== undefined && (!Number.isFinite(p.thicknessM) || Math.abs(p.thicknessM-thicknessM)>EPS))
    throw new RangeError(`this bond requires ${thicknessM} m wall thickness`);
  const maxBricks = p.maxBricks ?? 65536;
  if (!Number.isInteger(maxBricks) || maxBricks < 1 || maxBricks > 65536 ||
      columns*courses*(bond === 'stack' ? 1 : 2) > maxBricks)
    throw new RangeError('wall exceeds brick placement budget');
  const seed = p.seed ?? 0;
  if (!Number.isInteger(seed) || seed < 0 || seed > 0xffffffff) throw new RangeError('seed must be uint32');
  const hasModule = p.moduleColumns !== undefined || p.moduleCourses !== undefined;
  if (!hasModule && (p.phaseColumns !== undefined || p.phaseCourses !== undefined))
    throw new RangeError('module phase requires moduleColumns or moduleCourses');
  const surfaceModule = hasModule ? brickSurfaceModule(p) : null;
  const widthM = columns*l + (columns-1)*head, heightM = courses*h + (courses-1)*bed;
  const recess = positive(p.mortarRecessM ?? .008, 'mortar recess');
  if (recess*2 >= Math.min(l,h,d)) throw new RangeError('mortar recess must fit inside a brick');
  const placements = [], mortar = [];
  const put = (course, wythe, column, role, x, z, header = false) => {
    const id = header ? `c${course}/${role}` : `c${course}/w${wythe}/b${column}`;
    const surfaceCell = surfaceModule ? brickModuleAddress(surfaceModule,course,column,wythe,role) : null;
    const appearanceSeed = surfaceCell ? surfaceCell.appearanceSeed : hash(seed, id), variant = appearanceSeed % 8;
    const y = course*(h+bed), sx = header ? d : l, sz = header ? l : d;
    // Source origin is the brick's bottom centre. Exact quarter-turn, no scale.
    const matrix = header ? [0,0,1,x, 0,1,0,y, -1,0,0,z, 0,0,0,1]
                          : [1,0,0,x, 0,1,0,y, 0,0,1,z, 0,0,0,1];
    placements.push({ id, course, wythe, column, role, variant, appearanceSeed, matrix,
      ...(surfaceCell ? {surfaceCell} : {}),
      bounds: { min: [x-sx/2,y,z-sz/2], max: [x+sx/2,y+h,z+sz/2] } });
  };
  const gap = (id, min, max) => {
    if (max.every((n,i) => n > min[i]+EPS)) mortar.push({ id, min, max });
  };
  for (let row=0; row<courses; ++row) {
    const y = row*(h+bed), bottom = y + (row===0 ? recess : 0);
    const top = y+h - (row===courses-1 ? recess : 0);
    if (row+1<courses) gap(`bed-${row}`, [recess,y+h,recess], [widthM-recess,y+h+bed,thicknessM-recess]);
    if (bond === 'stack') {
      for (let col=0; col<columns; ++col) put(row,0,col,'stretcher',col*(l+head)+l/2,d/2);
      for (let col=0; col+1<columns; ++col)
        gap(`head-${row}-${col}`, [col*(l+head)+l,bottom,recess], [(col+1)*(l+head),top,d-recess]);
    } else if (!(row&1)) {
      for (let wythe=0; wythe<2; ++wythe)
        for (let col=0; col<columns; ++col) put(row,wythe,col,'stretcher',col*(l+head)+l/2,wythe*(d+head)+d/2);
      gap(`core-${row}`, [recess,bottom,d], [widthM-recess,top,d+head]);
      for (let col=0; col+1<columns; ++col)
        gap(`head-${row}-${col}`, [col*(l+head)+l,bottom,recess], [(col+1)*(l+head),top,l-recess]);
    } else {
      put(row,-1,0,'header-left',d/2,l/2,true);
      put(row,-1,columns,'header-right',widthM-d/2,l/2,true);
      for (let wythe=0; wythe<2; ++wythe)
        for (let col=0; col+1<columns; ++col)
          put(row,wythe,col,'stretcher',d+head+col*(l+head)+l/2,wythe*(d+head)+d/2);
      gap(`core-${row}`, [d,bottom,d], [widthM-d,top,d+head]);
      for (let col=0; col<columns; ++col)
        gap(`head-${row}-${col}`, [d+col*(l+head),bottom,recess], [d+col*(l+head)+head,top,l-recess]);
    }
  }
  return { version: 1, bond, brick, headJointM: head, bedJointM: bed, columns, courses,
    widthM, heightM, thicknessM, fit, requested: { widthM: p.widthM ?? null, heightM: p.heightM ?? null },
    adjustmentM: { width: p.widthM === undefined ? 0 : widthM-p.widthM,
      height: p.heightM === undefined ? 0 : heightM-p.heightM },
    sourceFaces: sourceFaces(l,h,d), placements, mortar,
    ...(surfaceModule ? {surfaceModule} : {}),
    bounds: { min: [0,0,0], max: [widthM,heightM,thicknessM] } };
}
