// Deterministic metre-grid topology compiler for the reusable castle kit.
// This module deliberately has no World/Part dependency so it runs unchanged
// under QuickJS and Node-based topology tests.

export const CASTLE_PLAN_SCHEMA = 'matter.castle-plan/v1';
export const CASTLE_MANIFEST_SCHEMA = 'matter.castle-manifest/v1';

const CARDINAL = Object.freeze({
  E: [1, 0], N: [0, 1], W: [-1, 0], S: [0, -1],
});
const CARDINAL_ANGLE = Object.freeze({ E: 0, N: 90, W: 180, S: 270 });
const EDGE_KINDS = new Set(['wall', 'open', 'door', 'window', 'arch']);
const PORTAL_KINDS = new Set(['open', 'door', 'arch']);
const MODULE_LENGTHS = [8, 4, 2, 1];
const MIN_PORTAL_WIDTH = 1.2;
const MIN_PORTAL_HEIGHT = 2.1;

function fail(path, message) {
  throw new Error(`castle plan ${path}: ${message}`);
}

function finite(value, path) {
  if (typeof value !== 'number' || !Number.isFinite(value)) fail(path, 'must be finite');
  return value;
}

function integer(value, path) {
  finite(value, path);
  if (!Number.isInteger(value)) fail(path, 'must be an integer metre-grid coordinate');
  return value;
}

function string(value, path) {
  if (typeof value !== 'string' || value.length === 0) fail(path, 'must be a non-empty string');
  return value;
}

function cmp(a, b) {
  return a < b ? -1 : a > b ? 1 : 0;
}

function stableSort(values, key) {
  return [...values].sort((a, b) => cmp(key(a), key(b)));
}

function idToken(value) {
  return encodeURIComponent(String(value)).replace(/%/g, '~');
}

function point2(value, path, requireInteger = true) {
  if (!Array.isArray(value) || value.length !== 2) fail(path, 'must be [x,z]');
  const check = requireInteger ? integer : finite;
  return [check(value[0], `${path}[0]`), check(value[1], `${path}[1]`)];
}

function point3(value, path) {
  if (!Array.isArray(value) || value.length !== 3) fail(path, 'must be [x,y,z]');
  return value.map((v, i) => finite(v, `${path}[${i}]`));
}

function cellKey(x, z) { return `${x},${z}`; }
function vertexKey(x, z) { return `${x},${z}`; }

function comparePoint(a, b) {
  return a[0] - b[0] || a[1] - b[1];
}

export function canonicalEdge(from, to) {
  const a = point2(from, 'edge.from');
  const b = point2(to, 'edge.to');
  if (Math.abs(a[0] - b[0]) + Math.abs(a[1] - b[1]) !== 1)
    fail('edge', 'must span exactly one axis-aligned grid metre');
  const ordered = comparePoint(a, b) <= 0 ? [a, b] : [b, a];
  return {
    from: ordered[0],
    to: ordered[1],
    key: `${ordered[0][0]},${ordered[0][1]}|${ordered[1][0]},${ordered[1][1]}`,
    axis: ordered[0][0] === ordered[1][0] ? 'z' : 'x',
  };
}

function expandSegment(from, to, path) {
  const a = point2(from, `${path}.from`);
  const b = point2(to, `${path}.to`);
  if ((a[0] === b[0]) === (a[1] === b[1])) fail(path, 'must be a non-zero axis-aligned segment');
  const axis = a[0] === b[0] ? 1 : 0;
  const sign = Math.sign(b[axis] - a[axis]);
  const length = Math.abs(b[axis] - a[axis]);
  const edges = [];
  for (let i = 0; i < length; ++i) {
    const p = [...a], q = [...a];
    p[axis] += sign * i;
    q[axis] += sign * (i + 1);
    edges.push(canonicalEdge(p, q));
  }
  return { from: a, to: b, axis: axis === 0 ? 'x' : 'z', length, edges };
}

function roomCells(room, path) {
  if (room.rect !== undefined && room.cells !== undefined)
    fail(path, 'choose rect or cells, not both');
  if (room.boundary !== undefined) {
    if (room.rect !== undefined || room.cells !== undefined)
      fail(path, 'a circular boundary cannot also declare grid cells');
    if (room.boundary.kind !== 'circle') fail(`${path}.boundary.kind`, 'must be circle');
    point2(room.boundary.center, `${path}.boundary.center`);
    const radius = finite(room.boundary.radius, `${path}.boundary.radius`);
    if (radius <= 0 || !Number.isInteger(radius)) fail(`${path}.boundary.radius`, 'must be a positive integer');
    return [];
  }
  let cells = [];
  if (room.rect !== undefined) {
    const rect = room.rect;
    if (!rect || typeof rect !== 'object') fail(`${path}.rect`, 'must be an object');
    const x = integer(rect.x, `${path}.rect.x`), z = integer(rect.z, `${path}.rect.z`);
    const width = integer(rect.width, `${path}.rect.width`);
    const depth = integer(rect.depth, `${path}.rect.depth`);
    if (width <= 0 || depth <= 0) fail(`${path}.rect`, 'width and depth must be positive');
    for (let dz = 0; dz < depth; ++dz)
      for (let dx = 0; dx < width; ++dx) cells.push([x + dx, z + dz]);
  } else if (Array.isArray(room.cells) && room.cells.length > 0) {
    cells = room.cells.map((cell, index) => point2(cell, `${path}.cells[${index}]`));
  } else {
    fail(path, 'must declare rect, non-empty cells, or a circle boundary');
  }
  const seen = new Set();
  for (const cell of cells) {
    const key = cellKey(cell[0], cell[1]);
    if (seen.has(key)) fail(`${path}.cells`, `duplicates cell ${key}`);
    seen.add(key);
  }
  const pending = [cells[0]], reached = new Set([cellKey(cells[0][0], cells[0][1])]);
  while (pending.length) {
    const [x, z] = pending.pop();
    for (const [dx, dz] of Object.values(CARDINAL)) {
      const key = cellKey(x + dx, z + dz);
      if (seen.has(key) && !reached.has(key)) { reached.add(key); pending.push([x + dx, z + dz]); }
    }
  }
  if (reached.size !== cells.length) fail(`${path}.cells`, 'must form one edge-connected cell union');
  return [...cells].sort((a, b) => a[0] - b[0] || a[1] - b[1]);
}

function roomBoundary(room) {
  if (!room.boundary) return { kind: 'cells', cells: room._cells.map(cell => [...cell]) };
  return {
    kind: 'circle',
    center: [...room.boundary.center],
    radius: room.boundary.radius,
  };
}

function edgeForCell(x, z, side) {
  if (side === 'N') return canonicalEdge([x, z + 1], [x + 1, z + 1]);
  if (side === 'S') return canonicalEdge([x, z], [x + 1, z]);
  if (side === 'E') return canonicalEdge([x + 1, z], [x + 1, z + 1]);
  return canonicalEdge([x, z], [x, z + 1]);
}

function defaultSection(style, level) {
  return {
    thickness: style.wallThickness ?? 0.6,
    height: level.height,
    material: style.wallMaterial ?? 'castle.stone',
    bond: style.bond ?? 'ashlar',
    profile: style.exteriorProfile ?? 'exterior',
  };
}

function normalizeOpening(override, segment, path) {
  if (!PORTAL_KINDS.has(override.kind) && override.kind !== 'window') return [];
  const raw = override.opening || {};
  const width = finite(raw.width ?? segment.length, `${path}.opening.width`);
  if (width <= 0 || width > segment.length) fail(`${path}.opening.width`, `must be in (0, ${segment.length}]`);
  const offset = finite(raw.offset ?? (segment.length - width) / 2, `${path}.opening.offset`);
  if (offset < 0 || offset + width > segment.length) fail(`${path}.opening`, 'interval exceeds override segment');
  const bottom = finite(raw.bottom ?? (override.kind === 'window' ? 1 : 0), `${path}.opening.bottom`);
  const defaultHeight = override.kind === 'window' ? 1.4 :
    override.kind === 'open' ? override.wallHeight : 2.2;
  const height = finite(raw.height ?? defaultHeight, `${path}.opening.height`);
  if (bottom < 0 || height <= 0 || bottom + height > override.wallHeight)
    fail(`${path}.opening`, 'vertical opening must fit within the wall height');
  if (PORTAL_KINDS.has(override.kind)) {
    if (width < MIN_PORTAL_WIDTH) fail(`${path}.opening.width`, `circulation portal must be at least ${MIN_PORTAL_WIDTH}m wide`);
    if (bottom !== 0) fail(`${path}.opening.bottom`, 'circulation portal must start at the walkable floor');
    if (height < MIN_PORTAL_HEIGHT) fail(`${path}.opening.height`, `circulation portal must be at least ${MIN_PORTAL_HEIGHT}m high`);
  }
  if (override.kind === 'open' && width !== segment.length)
    fail(`${path}.opening.width`, 'open socket boundaries must open the full override span');
  const axis = segment.axis === 'x' ? 0 : 1;
  const descending = segment.to[axis] < segment.from[axis];
  const canonicalOffset = descending ? segment.length - offset - width : offset;
  const segmentFrom = comparePoint(segment.from, segment.to) <= 0 ? segment.from : segment.to;
  const segmentTo = comparePoint(segment.from, segment.to) <= 0 ? segment.to : segment.from;
  return [{
    apertureId: `aperture:${override.id}`,
    offset: canonicalOffset, width, bottom, height, kind: override.kind,
    segmentFrom: [...segmentFrom], segmentTo: [...segmentTo],
    globalStart: canonicalOffset, globalEnd: canonicalOffset + width,
  }];
}

function buildOverrides(level, roomsById) {
  const result = new Map();
  const ids = new Set();
  for (const [index, override] of (level.edgeOverrides || []).entries()) {
    const path = `levels.${level.id}.edgeOverrides[${index}]`;
    if (!override || typeof override !== 'object') fail(path, 'must be an object');
    const kind = override.kind ?? 'wall';
    if (!EDGE_KINDS.has(kind)) fail(`${path}.kind`, `must be one of ${[...EDGE_KINDS].join(', ')}`);
    const segment = expandSegment(override.from, override.to, path);
    const id = override.id === undefined
      ? `override:${level.id}:${segment.from.join(',')}:${segment.to.join(',')}:${kind}`
      : string(override.id, `${path}.id`);
    if (ids.has(id)) fail(`${path}.id`, `duplicate id ${id}`);
    ids.add(id);
    const openings = normalizeOpening({
      ...override, id: `${level.id}:${id}`, kind, wallHeight: level.height,
    }, segment, path);
    let connects = override.connects;
    if (connects !== undefined) {
      if (!Array.isArray(connects) || connects.length !== 2) fail(`${path}.connects`, 'must contain exactly two room ids');
      connects = connects.map((roomId, i) => string(roomId, `${path}.connects[${i}]`));
      for (const roomId of connects)
        if (roomId !== 'outside' && !roomsById.has(roomId)) fail(`${path}.connects`, `unknown room ${roomId}`);
      if (!PORTAL_KINDS.has(kind)) fail(`${path}.connects`, `${kind} is not a circulation portal`);
    }
    const axisIndex = segment.axis === 'x' ? 0 : 1;
    const canonicalOrigin = Math.min(segment.from[axisIndex], segment.to[axisIndex]);
    const canonicalEdges = stableSort(segment.edges, edge => `${edge.from[axisIndex]}`);
    const affected = [];
    for (const edge of canonicalEdges) {
      const edgeIndex = edge.from[axisIndex] - canonicalOrigin;
      if (result.has(edge.key)) fail(path, `overlaps override ${result.get(edge.key).id}`);
      const localOpenings = [];
      for (const opening of openings) {
        const start = Math.max(opening.offset, edgeIndex);
        const end = Math.min(opening.offset + opening.width, edgeIndex + 1);
        if (end > start) localOpenings.push({
          apertureId: opening.apertureId,
          kind: opening.kind,
          start: start - edgeIndex,
          end: end - edgeIndex,
          localStart: start - edgeIndex,
          localEnd: end - edgeIndex,
          segmentFrom: [...opening.segmentFrom],
          segmentTo: [...opening.segmentTo],
          globalStart: opening.globalStart,
          globalEnd: opening.globalEnd,
          bottom: opening.bottom,
          top: opening.bottom + opening.height,
        });
      }
      affected.push({ edge, localOpenings });
    }
    const apertureOwnerEdgeId = affected
      .filter(item => item.localOpenings.length > 0)
      .map(item => `wall:${idToken(level.id)}:${item.edge.key}`)
      .sort()[0] ?? null;
    for (const { edge, localOpenings } of affected) {
      for (const opening of localOpenings) opening.ownerEdgeId = apertureOwnerEdgeId;
      result.set(edge.key, {
        id, kind, connects, segmentLength: segment.length,
        material: override.material,
        bond: override.bond,
        profile: override.profile,
        thickness: override.thickness,
        railProfile: override.railProfile ?? null,
        openings: localOpenings,
      });
    }
  }
  return result;
}

function wallDirection(edge, atStart) {
  const dx = edge.to[0] - edge.from[0], dz = edge.to[1] - edge.from[1];
  return atStart ? [dx, dz] : [-dx, -dz];
}

function directionName(vector) {
  if (vector[0] === 1) return 'E';
  if (vector[0] === -1) return 'W';
  if (vector[1] === 1) return 'N';
  return 'S';
}

function classifyJunction(directions) {
  const dirs = new Set(directions);
  if (dirs.size === 1) return 'end';
  if (dirs.size === 2) {
    const values = [...dirs];
    const a = CARDINAL[values[0]], b = CARDINAL[values[1]];
    return a[0] + b[0] === 0 && a[1] + b[1] === 0 ? 'straight' : 'L';
  }
  if (dirs.size === 3) return 'T';
  if (dirs.size === 4) return 'cross';
  fail('junction', `has invalid degree ${dirs.size}`);
}

function buildWalls(level, rooms, style) {
  const roomsById = new Map(rooms.map(room => [room.id, room]));
  const cellOwners = new Map();
  for (const room of rooms) {
    for (const cell of room._cells) {
      const key = cellKey(cell[0], cell[1]);
      if (cellOwners.has(key))
        fail(`levels.${level.id}.rooms.${room.id}`, `cell ${key} overlaps room ${cellOwners.get(key)}`);
      cellOwners.set(key, room.id);
    }
  }
  const candidates = new Map();
  for (const room of rooms) {
    for (const [x, z] of room._cells) {
      for (const side of ['N', 'E', 'S', 'W']) {
        const delta = CARDINAL[side];
        const neighbor = cellOwners.get(cellKey(x + delta[0], z + delta[1]));
        if (neighbor === room.id) continue;
        const edge = edgeForCell(x, z, side);
        let candidate = candidates.get(edge.key);
        if (!candidate) {
          candidate = { ...edge, roomIds: new Set() };
          candidates.set(edge.key, candidate);
        }
        candidate.roomIds.add(room.id);
      }
    }
  }
  const overrides = buildOverrides(level, roomsById);
  for (const key of overrides.keys())
    if (!candidates.has(key)) fail(`levels.${level.id}.edgeOverrides`, `edge ${key} is not a room boundary`);
  const walls = [];
  for (const candidate of stableSort(candidates.values(), value => value.key)) {
    const roomIds = [...candidate.roomIds].sort();
    const override = overrides.get(candidate.key);
      const section = defaultSection(style, level);
    const kind = override?.kind ?? 'wall';
    const connects = override?.connects ??
      (PORTAL_KINDS.has(kind) && kind !== 'open' && roomIds.length === 2 ? roomIds : undefined);
    if (connects) {
      if (connects.some(roomId => roomId !== 'outside' && roomsById.get(roomId)?.openToBelow === true))
        fail(`levels.${level.id}.edgeOverrides.${override.id}`,
          'cannot create a circulation portal into an openToBelow air room');
      const expected = roomIds.length === 2 ? [...roomIds].sort() : [...roomIds, 'outside'].sort();
      if (connects.length !== expected.length || [...connects].sort().some((roomId, i) => roomId !== expected[i]))
        fail(`levels.${level.id}.edgeOverrides.${override.id}`,
          `connects rooms that are not physically adjacent; expected ${expected.join(',')}, got ${connects.join(',')}`);
    }
    const interior = roomIds.length === 2;
    const thickness = finite(override?.thickness ?? section.thickness, `wall ${candidate.key}.thickness`);
    if (thickness <= 0) fail(`wall ${candidate.key}.thickness`, 'must be positive');
    walls.push({
      id: `wall:${idToken(level.id)}:${candidate.key}`,
      levelId: level.id,
      from: candidate.from,
      to: candidate.to,
      axis: candidate.axis,
      roomIds,
      boundary: interior ? 'partition' : 'exterior',
      kind,
      overrideId: override?.id ?? null,
      connects: connects ? [...connects].sort() : null,
      section: {
        thickness,
        height: level.height,
        material: override?.material ?? section.material,
        bond: override?.bond ?? section.bond,
        profile: override?.profile ?? (interior ? (style.interiorProfile ?? 'partition') : section.profile),
      },
      openings: override?.openings ?? [],
      railProfile: override?.railProfile ?? null,
    });
  }
  return { cellOwners, walls };
}

function addJunctions(walls, level) {
  const incident = new Map();
  for (const wall of walls) {
    if (wall.kind === 'open') continue;
    for (const [point, atStart] of [[wall.from, true], [wall.to, false]]) {
      const key = vertexKey(point[0], point[1]);
      if (!incident.has(key)) incident.set(key, []);
      incident.get(key).push({ wall, direction: directionName(wallDirection(wall, atStart)) });
    }
  }
  const junctions = [];
  for (const [key, entries] of stableSort(incident.entries(), entry => entry[0])) {
    const directions = entries.map(entry => entry.direction).sort();
    const kind = classifyJunction(directions);
    const edgeIds = entries.map(entry => entry.wall.id).sort();
    const ownerEdgeId = edgeIds[0];
    const position = key.split(',').map(Number);
    const id = `junction:${idToken(level.id)}:${key}`;
    const thickness = Math.max(...entries.map(entry => entry.wall.section.thickness));
    const trim = kind === 'straight' ? 0 : thickness / 2;
    const ownedVolume = kind === 'straight' ? null : {
      minX: position[0] - thickness / 2, maxX: position[0] + thickness / 2,
      minY: level.baseY, maxY: level.baseY + level.height,
      minZ: position[1] - thickness / 2, maxZ: position[1] + thickness / 2,
    };
    junctions.push({ id, levelId: level.id, position, kind, directions, edgeIds, ownerEdgeId, ownedVolume });
    for (const entry of entries) {
      const socket = {
        id: `socket:${entry.wall.id}:${key}`,
        vertexId: id,
        position: [...position],
        direction: entry.direction,
        junctionKind: kind,
        owner: entry.wall.id === ownerEdgeId,
        trim,
        ownsJunctionVolume: entry.wall.id === ownerEdgeId && ownedVolume !== null,
        section: { ...entry.wall.section },
      };
      if (comparePoint(entry.wall.from, position) === 0) entry.wall.startSocket = socket;
      else entry.wall.endSocket = socket;
    }
  }
  for (const wall of walls) wall.trim = {
    start: wall.startSocket?.trim ?? 0,
    end: wall.endSocket?.trim ?? 0,
  };
  return junctions;
}

function moduleSignature(wall) {
  return JSON.stringify([wall.levelId, wall.axis, wall.kind, wall.boundary, wall.section,
    wall.overrideId, wall.railProfile, wall.openings]);
}

function mergeWallModules(walls, junctionByVertex) {
  const groups = new Map();
  for (const wall of walls.filter(candidate =>
    candidate.kind !== 'open' && candidate.suppressedByRadialThroat !== true)) {
    const line = wall.axis === 'x' ? wall.from[1] : wall.from[0];
    const key = `${moduleSignature(wall)}:${line}`;
    if (!groups.has(key)) groups.set(key, []);
    groups.get(key).push(wall);
  }
  const modules = [];
  function emitChain(chain) {
    let offset = 0;
    while (offset < chain.length) {
      const length = MODULE_LENGTHS.find(size => size <= chain.length - offset);
      const slice = chain.slice(offset, offset + length);
      modules.push({
        id: `module:${slice.map(wall => wall.id.slice(5)).join('+')}`,
        levelId: slice[0].levelId,
        kind: slice[0].kind,
        length,
        from: [...slice[0].from],
        to: [...slice[slice.length - 1].to],
        section: { ...slice[0].section },
        sourceEdgeIds: slice.map(wall => wall.id),
        apertureIds: [...new Set(slice.flatMap(wall => wall.openings.map(opening => opening.apertureId)))].sort(),
        apertures: stableSort(slice.flatMap(wall => wall.openings)
          .filter((opening, index, all) => all.findIndex(candidate => candidate.apertureId === opening.apertureId) === index)
          .map(opening => ({
            apertureId: opening.apertureId,
            kind: opening.kind,
            segmentFrom: [...opening.segmentFrom], segmentTo: [...opening.segmentTo],
            globalStart: opening.globalStart, globalEnd: opening.globalEnd,
            bottom: opening.bottom, top: opening.top, ownerEdgeId: opening.ownerEdgeId,
            claimedByRadialThroatId: opening.claimedByRadialThroatId ?? null,
          })), opening => opening.apertureId),
        ownsAperture: slice.some(wall => wall.openings.some(opening => opening.ownerEdgeId === wall.id)),
        trim: { start: slice[0].trim.start, end: slice[slice.length - 1].trim.end },
        ownedJunctionIds: [...new Set(slice.flatMap(wall => [wall.startSocket, wall.endSocket]
          .filter(socket => socket?.ownsJunctionVolume).map(socket => socket.vertexId)))].sort(),
      });
      offset += length;
    }
  }
  for (const [, sourceGroup] of stableSort(groups.entries(), entry => entry[0])) {
    const group = [...sourceGroup].sort((a, b) =>
      (a.axis === 'x' ? a.from[0] - b.from[0] : a.from[1] - b.from[1]) || cmp(a.id, b.id));
    let chain = [];
    for (const wall of group) {
      const previous = chain[chain.length - 1];
      const junction = previous && junctionByVertex.get(
        `${previous.levelId}:${vertexKey(previous.to[0], previous.to[1])}`);
      if (previous && (comparePoint(previous.to, wall.from) !== 0 || junction?.kind !== 'straight')) {
        emitChain(chain);
        chain = [];
      }
      chain.push(wall);
    }
    if (chain.length) emitChain(chain);
  }
  return stableSort(modules, module => module.id);
}

function tangentFor(cardinal, clockwise) {
  const radial = CARDINAL[cardinal];
  const tangent = clockwise ? [radial[1], -radial[0]] : [-radial[1], radial[0]];
  return tangent.map(value => value === 0 ? 0 : value);
}

function curveEndpoint(center, radius, cardinal) {
  const radial = CARDINAL[cardinal];
  return [center[0] + radial[0] * radius, center[1] + radial[1] * radius];
}

function sectionCompatible(a, b) {
  return a.thickness === b.thickness && a.height === b.height &&
    a.material === b.material && a.bond === b.bond;
}

function boundsFromRect(rect) {
  return { minX: rect.x, maxX: rect.x + rect.width, minZ: rect.z, maxZ: rect.z + rect.depth };
}

function rectFromBounds(bounds) {
  return { x: bounds.minX, z: bounds.minZ, width: bounds.maxX - bounds.minX, depth: bounds.maxZ - bounds.minZ };
}

function roomCoversBounds(room, bounds) {
  if (room.boundary?.kind === 'circle') {
    const [cx, cz] = room.boundary.center, radius2 = room.boundary.radius ** 2;
    return [[bounds.minX, bounds.minZ], [bounds.minX, bounds.maxZ],
      [bounds.maxX, bounds.minZ], [bounds.maxX, bounds.maxZ]]
      .every(([x, z]) => (x - cx) ** 2 + (z - cz) ** 2 <= radius2 + 1e-9);
  }
  const cells = new Set(room._cells.map(cell => cellKey(cell[0], cell[1])));
  const maxCellX = Math.ceil(bounds.maxX - 1e-9);
  const maxCellZ = Math.ceil(bounds.maxZ - 1e-9);
  for (let z = Math.floor(bounds.minZ); z < maxCellZ; ++z)
    for (let x = Math.floor(bounds.minX); x < maxCellX; ++x)
      if (!cells.has(cellKey(x, z))) return false;
  return true;
}

function roomsCoverBounds(rooms, bounds) {
  if (rooms.some(room => roomCoversBounds(room, bounds))) return true;
  const cells = new Set(rooms.flatMap(room => room._cells)
    .map(([x, z]) => cellKey(x, z)));
  for (let z = Math.floor(bounds.minZ); z < Math.ceil(bounds.maxZ - 1e-9); ++z)
    for (let x = Math.floor(bounds.minX); x < Math.ceil(bounds.maxX - 1e-9); ++x)
      if (!cells.has(cellKey(x, z))) return false;
  return cells.size > 0;
}

function pointInsideRoom(room, point, margin = 0) {
  const x = point[0], z = point.length === 3 ? point[2] : point[1];
  if (room.boundary?.kind === 'circle') {
    const dx = x - room.boundary.center[0], dz = z - room.boundary.center[1];
    return Math.hypot(dx, dz) <= room.boundary.radius - margin + 1e-9;
  }
  return room._cells.some(cell =>
    x >= cell[0] + margin - 1e-9 && x <= cell[0] + 1 - margin + 1e-9 &&
    z >= cell[1] + margin - 1e-9 && z <= cell[1] + 1 - margin + 1e-9);
}

function pointInsideFloorHole(point, holes) {
  return holes.some(hole => hole.replacementLandingId === null && (() => {
    const region = hole.footprint;
    return point[0] > region.x + 1e-9 && point[0] < region.x + region.width - 1e-9 &&
      point[2] > region.z + 1e-9 && point[2] < region.z + region.depth - 1e-9;
  })());
}

function segmentEntersExpandedRect(from, to, rect, radius) {
  const epsilon = 1e-7;
  const mins = [rect.x - radius, rect.z - radius];
  const maxs = [rect.x + rect.width + radius, rect.z + rect.depth + radius];
  const starts = [from[0], from[2]], deltas = [to[0] - from[0], to[2] - from[2]];
  let first = 0, last = 1;
  for (let axis = 0; axis < 2; ++axis) {
    if (Math.abs(deltas[axis]) < 1e-9) {
      if (starts[axis] <= mins[axis] + epsilon || starts[axis] >= maxs[axis] - epsilon)
        return false;
      continue;
    }
    const a = (mins[axis] - starts[axis]) / deltas[axis];
    const b = (maxs[axis] - starts[axis]) / deltas[axis];
    first = Math.max(first, Math.min(a, b));
    last = Math.min(last, Math.max(a, b));
  }
  return Math.max(first, epsilon) < Math.min(last, 1 - epsilon) - epsilon;
}

function roomSegmentLeg(room, from, to, width, holes) {
  const dx = to[0] - from[0], dz = to[2] - from[2];
  const length = Math.hypot(dx, dz);
  if (length < 1e-9) return null;
  const px = length > 1e-9 ? -dz / length : 0;
  const pz = length > 1e-9 ? dx / length : 0;
  const bounds = {
    minX: Math.min(from[0], to[0]) - Math.abs(px) * width / 2,
    maxX: Math.max(from[0], to[0]) + Math.abs(px) * width / 2,
    minY: Math.min(from[1], to[1]), maxY: Math.max(from[1], to[1]) + MIN_PORTAL_HEIGHT,
    minZ: Math.min(from[2], to[2]) - Math.abs(pz) * width / 2,
    maxZ: Math.max(from[2], to[2]) + Math.abs(pz) * width / 2,
  };
  if (holes.some(hole => hole.replacementLandingId === null &&
      segmentEntersExpandedRect(from, to, hole.footprint, width / 2))) return null;
  const samples = Math.max(1, Math.ceil(length / 0.2));
  for (let i = 0; i <= samples; ++i) {
    const t = i / samples;
    const x = from[0] + dx * t, z = from[2] + dz * t;
    for (const offset of [-width / 2, 0, width / 2])
      if (!pointInsideRoom(room, [x + px * offset, z + pz * offset]) ||
          pointInsideFloorHole([x + px * offset, from[1], z + pz * offset], holes))
        return null;
  }
  return {
    from: [...from], to: [...to],
    bounds,
  };
}

function validateRoomSegment(room, from, to, width, holes, path) {
  const y = from[1];
  const inset = width / 2;
  const makeRecord = (waypoints, legs) => ({
    roomId: room.id, from: [...from], to: [...to], width, waypoints,
    segments: legs,
    sweptBounds: {
      minX: Math.min(...legs.map(leg => leg.bounds.minX)),
      maxX: Math.max(...legs.map(leg => leg.bounds.maxX)),
      minY: Math.min(...legs.map(leg => leg.bounds.minY)),
      maxY: Math.max(...legs.map(leg => leg.bounds.maxY)),
      minZ: Math.min(...legs.map(leg => leg.bounds.minZ)),
      maxZ: Math.max(...legs.map(leg => leg.bounds.maxZ)),
    },
  });
  const endpointCandidates = point => [[...point], ...Object.values(CARDINAL).map(([dx, dz]) =>
    [point[0] + dx * inset, point[1], point[2] + dz * inset])];
  const candidates = [];
  for (const a of endpointCandidates(from))
    for (const b of endpointCandidates(to)) {
      candidates.push([[...from], a, [b[0], y, a[2]], b, [...to]]);
      candidates.push([[...from], a, [a[0], y, b[2]], b, [...to]]);
    }
  for (const candidate of candidates) {
    const waypoints = candidate.filter((point, index) => index === 0 ||
      point.some((value, coordinate) => Math.abs(value - candidate[index - 1][coordinate]) > 1e-9));
    const legs = [];
    let valid = true;
    for (let i = 0; i + 1 < waypoints.length; ++i) {
      const leg = roomSegmentLeg(room, waypoints[i], waypoints[i + 1], width, holes);
      if (!leg) { valid = false; break; }
      legs.push(leg);
    }
    if (!valid || legs.length === 0) continue;
    return makeRecord(waypoints, legs);
  }

  // When a stair flight or authored shaft blocks both simple L routes, build a
  // small deterministic rectilinear visibility graph from floor-cell lanes and
  // inflated obstacle corners. Adjacent collinear nodes are sufficient because
  // every edge is validated as a complete swept strip.
  const xs = new Set([from[0], to[0]]), zs = new Set([from[2], to[2]]);
  for (const point of [...endpointCandidates(from), ...endpointCandidates(to)]) {
    xs.add(point[0]); zs.add(point[2]);
  }
  for (const hole of holes.filter(candidate => candidate.replacementLandingId === null)) {
    xs.add(hole.footprint.x - inset); xs.add(hole.footprint.x + hole.footprint.width + inset);
    zs.add(hole.footprint.z - inset); zs.add(hole.footprint.z + hole.footprint.depth + inset);
  }
  for (const [x, z] of room._cells) {
    xs.add(x + inset); xs.add(x + 1 - inset);
    zs.add(z + inset); zs.add(z + 1 - inset);
  }
  if (room.boundary?.kind === 'circle') {
    const [cx, cz] = room.boundary.center, radius = room.boundary.radius - inset;
    xs.add(cx); xs.add(cx - radius); xs.add(cx + radius);
    zs.add(cz); zs.add(cz - radius); zs.add(cz + radius);
  }
  const keyFor = point => `${point[0]},${point[2]}`;
  const nodes = new Map();
  for (const x of [...xs].sort((a, b) => a - b))
    for (const z of [...zs].sort((a, b) => a - b)) {
      const point = [x, y, z];
      if ((x === from[0] && z === from[2]) || (x === to[0] && z === to[2]) ||
          (pointInsideRoom(room, point) && !pointInsideFloorHole(point, holes)))
        nodes.set(keyFor(point), point);
    }
  const adjacency = new Map([...nodes.keys()].map(key => [key, []]));
  const connectAdjacent = groups => {
    for (const group of groups.values()) {
      group.sort((a, b) => a[0] - b[0] || a[2] - b[2]);
      for (let i = 0; i + 1 < group.length; ++i) {
        if (!roomSegmentLeg(room, group[i], group[i + 1], width, holes)) continue;
        const a = keyFor(group[i]), b = keyFor(group[i + 1]);
        adjacency.get(a).push(b); adjacency.get(b).push(a);
      }
    }
  };
  const byX = new Map(), byZ = new Map();
  for (const point of nodes.values()) {
    if (!byX.has(point[0])) byX.set(point[0], []);
    if (!byZ.has(point[2])) byZ.set(point[2], []);
    byX.get(point[0]).push(point); byZ.get(point[2]).push(point);
  }
  connectAdjacent(byX); connectAdjacent(byZ);
  const startKey = keyFor(from), endKey = keyFor(to), queue = [startKey];
  const previous = new Map([[startKey, null]]);
  while (queue.length && !previous.has(endKey)) {
    const current = queue.shift();
    for (const next of adjacency.get(current).sort())
      if (!previous.has(next)) { previous.set(next, current); queue.push(next); }
  }
  if (previous.has(endKey)) {
    const raw = [];
    for (let cursor = endKey; cursor !== null; cursor = previous.get(cursor)) raw.push(nodes.get(cursor));
    raw.reverse();
    const waypoints = raw.filter((point, index) => index === 0 || index === raw.length - 1 ||
      (raw[index - 1][0] === point[0]) !== (point[0] === raw[index + 1][0]));
    const legs = waypoints.slice(0, -1).map((point, index) =>
      roomSegmentLeg(room, point, waypoints[index + 1], width, holes));
    if (legs.every(Boolean)) return makeRecord(waypoints, legs);
  }
  fail(path, `walk clearance leaves room ${room.id} floor`);
}

function rectanglesTouch(a, b) {
  const aa = boundsFromRect(a), bb = boundsFromRect(b);
  return aa.maxX + 1e-9 >= bb.minX && bb.maxX + 1e-9 >= aa.minX &&
    aa.maxZ + 1e-9 >= bb.minZ && bb.maxZ + 1e-9 >= aa.minZ;
}

function landingSupportsFlightEnd(landing, flight, atEnd) {
  const point = flight.centerline[atEnd ? 1 : 0];
  const bounds = boundsFromRect(landing);
  const half = flight.width / 2;
  if (flight.direction === 'E' || flight.direction === 'W')
    return point[0] >= bounds.minX - 1e-9 && point[0] <= bounds.maxX + 1e-9 &&
      point[2] - half >= bounds.minZ - 1e-9 && point[2] + half <= bounds.maxZ + 1e-9;
  return point[2] >= bounds.minZ - 1e-9 && point[2] <= bounds.maxZ + 1e-9 &&
    point[0] - half >= bounds.minX - 1e-9 && point[0] + half <= bounds.maxX + 1e-9;
}

function angleInside(angle, start, end) {
  return [angle - 360, angle, angle + 360].some(value => value >= start - 1e-9 && value <= end + 1e-9);
}

function radialThroatBounds(center, radius, direction, width, depth) {
  const [dx, dz] = CARDINAL[direction];
  const threshold = curveEndpoint(center, radius, direction);
  if (dx !== 0) return {
    minX: dx > 0 ? threshold[0] : threshold[0] - depth,
    maxX: dx > 0 ? threshold[0] + depth : threshold[0],
    minZ: threshold[1] - width / 2, maxZ: threshold[1] + width / 2,
  };
  return {
    minX: threshold[0] - width / 2, maxX: threshold[0] + width / 2,
    minZ: dz > 0 ? threshold[1] : threshold[1] - depth,
    maxZ: dz > 0 ? threshold[1] + depth : threshold[1],
  };
}

function radialBridgeBounds(center, radialDistance, direction, width, outerDistance) {
  const [dx, dz] = CARDINAL[direction];
  const near = [center[0] + dx * radialDistance, center[1] + dz * radialDistance];
  const far = [center[0] + dx * outerDistance, center[1] + dz * outerDistance];
  if (dx !== 0) return {
    minX: Math.min(near[0], far[0]), maxX: Math.max(near[0], far[0]),
    minZ: center[1] - width / 2, maxZ: center[1] + width / 2,
  };
  return {
    minX: center[0] - width / 2, maxX: center[0] + width / 2,
    minZ: Math.min(near[1], far[1]), maxZ: Math.max(near[1], far[1]),
  };
}

function openingWallSpan(opening, verticalPlane) {
  const axis = verticalPlane ? 1 : 0;
  return [opening.segmentFrom[axis] + opening.globalStart,
    opening.segmentFrom[axis] + opening.globalEnd];
}

function buildCurves(plan, levelsById, roomsById, walls) {
  const curves = [];
  const radialThroats = [];
  const curveTransitions = [];
  const seenIds = new Set();
  for (const [index, source] of (plan.curves || []).entries()) {
    const path = `curves[${index}]`;
    const id = string(source.id, `${path}.id`);
    if (seenIds.has(id)) fail(`${path}.id`, `duplicate id ${id}`);
    seenIds.add(id);
    const levelId = string(source.levelId, `${path}.levelId`);
    const level = levelsById.get(levelId);
    if (!level) fail(`${path}.levelId`, `unknown level ${levelId}`);
    const roomId = string(source.roomId, `${path}.roomId`);
    const room = roomsById.get(roomId);
    if (!room || room.levelId !== levelId) fail(`${path}.roomId`, `unknown room ${roomId} on level ${levelId}`);
    if (room.openToBelow) fail(`${path}.roomId`, 'curves cannot create circulation for an openToBelow air room');
    const center = point2(source.center, `${path}.center`);
    const radius = integer(source.radius, `${path}.radius`);
    if (radius <= 0) fail(`${path}.radius`, 'must be positive');
    const kind = source.kind ?? 'quarter';
    if (kind !== 'quarter' && kind !== 'ring') fail(`${path}.kind`, 'must be quarter or ring');
    const section = {
      thickness: finite(source.thickness ?? plan.style?.wallThickness ?? 0.6, `${path}.thickness`),
      height: finite(source.height ?? level.height, `${path}.height`),
      material: source.material ?? plan.style?.wallMaterial ?? 'castle.stone',
      bond: source.bond ?? plan.style?.bond ?? 'ashlar',
      profile: source.profile ?? 'curve',
    };
    if (section.thickness <= 0) fail(`${path}.thickness`, 'must be positive');
    if (section.height <= 0 || section.height > level.height) fail(`${path}.height`, 'must fit within its level height');
    let endpoints = [];
    if (kind === 'quarter') {
      const start = string(source.start, `${path}.start`);
      const end = string(source.end, `${path}.end`);
      if (!CARDINAL[start] || !CARDINAL[end]) fail(path, 'start/end must be E, N, W, or S');
      const delta = (CARDINAL_ANGLE[end] - CARDINAL_ANGLE[start] + 360) % 360;
      const clockwise = source.clockwise === true;
      if ((!clockwise && delta !== 90) || (clockwise && delta !== 270))
        fail(path, 'start/end must describe exactly one quarter turn in the chosen direction');
      endpoints = [
        { end: 'start', cardinal: start, position: curveEndpoint(center, radius, start), tangent: tangentFor(start, clockwise) },
        { end: 'end', cardinal: end, position: curveEndpoint(center, radius, end), tangent: tangentFor(end, clockwise) },
      ];
    }
    const seenApertures = new Set();
    const apertures = [...(source.apertures || []).map((aperture, apertureIndex) => {
      const aperturePath = `${path}.apertures[${apertureIndex}]`;
      const apertureKind = string(aperture.kind, `${aperturePath}.kind`);
      if (!EDGE_KINDS.has(apertureKind) || apertureKind === 'wall')
        fail(`${aperturePath}.kind`, 'must be open, door, window, or arch');
      let startAngle = finite(aperture.startAngle, `${aperturePath}.startAngle`);
      let endAngle = finite(aperture.endAngle, `${aperturePath}.endAngle`);
      if (endAngle <= startAngle) fail(aperturePath, 'endAngle must exceed startAngle');
      const angularSpan = endAngle - startAngle;
      if (angularSpan >= (kind === 'ring' ? 360 : 90))
        fail(aperturePath, 'aperture interval is too large for its curve');
      if (kind === 'ring') {
        startAngle = ((startAngle % 360) + 360) % 360;
        endAngle = startAngle + angularSpan;
      }
      if (kind === 'quarter') {
        const sweepStart = CARDINAL_ANGLE[source.start];
        const sweepEnd = source.clockwise === true ? sweepStart - 90 : sweepStart + 90;
        const low = Math.min(sweepStart, sweepEnd), high = Math.max(sweepStart, sweepEnd);
        const normalizeIntoSweep = angle => [angle - 720, angle - 360, angle,
          angle + 360, angle + 720].find(value => value >= low - 1e-9 && value <= high + 1e-9);
        const normalizedStart = normalizeIntoSweep(startAngle);
        const normalizedEnd = normalizeIntoSweep(endAngle);
        if (normalizedStart === undefined || normalizedEnd === undefined ||
            normalizedEnd <= normalizedStart)
          fail(aperturePath, 'aperture lies outside the quarter-curve sweep');
        startAngle = normalizedStart;
        endAngle = normalizedEnd;
      }
      let connects = null;
      if (aperture.connects !== undefined) {
        if (!PORTAL_KINDS.has(apertureKind)) fail(`${aperturePath}.connects`, 'windows are not circulation portals');
        if (!Array.isArray(aperture.connects) || aperture.connects.length !== 2)
          fail(`${aperturePath}.connects`, 'must contain exactly two room ids');
        connects = aperture.connects.map((value, i) => string(value, `${aperturePath}.connects[${i}]`)).sort();
        for (const target of connects) {
          const targetRoom = roomsById.get(target);
          if (target !== 'outside' && !targetRoom) fail(`${aperturePath}.connects`, `unknown room ${target}`);
          if (targetRoom && targetRoom.levelId !== levelId)
            fail(`${aperturePath}.connects`, `room ${target} is on another level`);
        }
        if (!connects.includes(roomId)) fail(`${aperturePath}.connects`, `must include curve room ${roomId}`);
      }
      const apertureId = aperture.id ?? `aperture:${id}:${startAngle}:${endAngle}`;
      if (seenApertures.has(apertureId)) fail(`${aperturePath}.id`, `duplicate id ${apertureId}`);
      seenApertures.add(apertureId);
      const bottom = finite(aperture.bottom ?? (apertureKind === 'window' ? 1 : 0), `${aperturePath}.bottom`);
      const height = finite(aperture.height ?? (apertureKind === 'window' ? 1.4 : 2.2), `${aperturePath}.height`);
      if (bottom < 0 || height <= 0 || bottom + height > section.height)
        fail(aperturePath, 'vertical opening must fit within the curved wall');
      if (PORTAL_KINDS.has(apertureKind) && (bottom !== 0 || height < MIN_PORTAL_HEIGHT))
        fail(aperturePath, `circulation portal must start at floor and be at least ${MIN_PORTAL_HEIGHT}m high`);
      let radialThroatId = null;
      if (aperture.throat !== undefined) {
        if (!connects || !PORTAL_KINDS.has(apertureKind)) fail(`${aperturePath}.throat`, 'requires a circulation connection');
        const throat = aperture.throat;
        const targetRoomId = string(throat.targetRoomId, `${aperturePath}.throat.targetRoomId`);
        const targetRoom = roomsById.get(targetRoomId);
        if (!targetRoom || targetRoom.levelId !== levelId || targetRoomId === roomId || !connects.includes(targetRoomId))
          fail(`${aperturePath}.throat.targetRoomId`, 'must be the connected same-level target room');
        if (targetRoom.openToBelow)
          fail(`${aperturePath}.throat.targetRoomId`, 'must be a walkable target room');
        const direction = string(throat.direction, `${aperturePath}.throat.direction`);
        if (!CARDINAL[direction]) fail(`${aperturePath}.throat.direction`, 'must be E, N, W, or S');
        const directionAngle = CARDINAL_ANGLE[direction];
        if (!angleInside(directionAngle, startAngle, endAngle))
          fail(`${aperturePath}.throat.direction`, 'radial direction must pass through the aperture interval');
        const width = finite(throat.width, `${aperturePath}.throat.width`);
        const depth = finite(throat.depth, `${aperturePath}.throat.depth`);
        const overlapDepth = finite(throat.overlapDepth ?? section.thickness / 2,
          `${aperturePath}.throat.overlapDepth`);
        if (width < MIN_PORTAL_WIDTH) fail(`${aperturePath}.throat.width`, `must be at least ${MIN_PORTAL_WIDTH}m`);
        if (depth <= 0) fail(`${aperturePath}.throat.depth`, 'must be positive');
        if (overlapDepth <= 0 || overlapDepth >= radius)
          fail(`${aperturePath}.throat.overlapDepth`, 'must be positive and smaller than the curve radius');
        if ((endAngle - startAngle) * Math.PI / 180 * radius + 1e-9 < width)
          fail(aperturePath, 'curved aperture arc is narrower than the throat');
        const targetBounds = radialThroatBounds(center, radius, direction, width, depth);
        if (!roomCoversBounds(targetRoom, targetBounds))
          fail(`${aperturePath}.throat`, `finite throat is not covered by target room ${targetRoomId} floor`);
        const wallThreshold = curveEndpoint(center, radius, direction);
        const usableRadius = radius - section.thickness / 2;
        if (width / 2 >= usableRadius)
          fail(`${aperturePath}.throat.width`, 'is too wide to overlap the usable circular floor');
        const chordDistance = Math.sqrt(usableRadius ** 2 - (width / 2) ** 2);
        const bridgeDistance = Math.min(chordDistance, radius - overlapDepth);
        const bounds = radialBridgeBounds(center, bridgeDistance, direction, width, radius + depth);
        const threshold = curveEndpoint(center, bridgeDistance, direction);
        const verticalPlane = direction === 'E' || direction === 'W';
        const targetWallEdges = walls.filter(wall => wall.levelId === levelId &&
          wall.roomIds.includes(targetRoomId) && wall.boundary === 'exterior' &&
          wall.axis === (verticalPlane ? 'z' : 'x') && sectionCompatible(section, wall.section) &&
          (verticalPlane ? wall.from[0] === wallThreshold[0] : wall.from[1] === wallThreshold[1]) &&
          (verticalPlane
            ? wall.to[1] > bounds.minZ && wall.from[1] < bounds.maxZ
            : wall.to[0] > bounds.minX && wall.from[0] < bounds.maxX));
        const coveredWidth = targetWallEdges.reduce((sum, wall) => sum +
          (verticalPlane
            ? Math.min(wall.to[1], bounds.maxZ) - Math.max(wall.from[1], bounds.minZ)
            : Math.min(wall.to[0], bounds.maxX) - Math.max(wall.from[0], bounds.minX)), 0);
        if (coveredWidth + 1e-9 < width)
          fail(`${aperturePath}.throat`,
            `target room ${targetRoomId} lacks a compatible boundary cut across the throat`);
        const hostApertures = new Map();
        for (const wall of targetWallEdges)
          for (const opening of wall.openings) {
            if (!PORTAL_KINDS.has(opening.kind) || opening.bottom !== 0 ||
                opening.top - opening.bottom + 1e-9 < height) continue;
            const [spanStart, spanEnd] = openingWallSpan(opening, verticalPlane);
            const throatStart = verticalPlane ? targetBounds.minZ : targetBounds.minX;
            const throatEnd = verticalPlane ? targetBounds.maxZ : targetBounds.maxX;
            if (spanStart <= throatStart + 1e-9 && spanEnd + 1e-9 >= throatEnd)
              hostApertures.set(opening.apertureId, opening);
          }
        const requestedHost = throat.hostApertureId === undefined ? null :
          string(throat.hostApertureId, `${aperturePath}.throat.hostApertureId`);
        const canonicalRequestedHost = requestedHost === null ? null :
          (requestedHost.startsWith('aperture:') ? requestedHost : `aperture:${levelId}:${requestedHost}`);
        const matchingHostIds = [...hostApertures.keys()].filter(candidate =>
          canonicalRequestedHost === null || candidate === canonicalRequestedHost).sort();
        if (matchingHostIds.length !== 1)
          fail(`${aperturePath}.throat.hostApertureId`, matchingHostIds.length === 0
            ? 'requires one matching target-wall door/arch/open aperture across the throat'
            : 'matches multiple target-wall apertures; specify hostApertureId');
        const hostApertureId = matchingHostIds[0];
        radialThroatId = `radial-throat:${id}:${apertureId}`;
        const farThreshold = [wallThreshold[0] + CARDINAL[direction][0] * depth,
          wallThreshold[1] + CARDINAL[direction][1] * depth];
        radialThroats.push({
          id: radialThroatId, kind: 'radial-room-entry', levelId, curveId: `curve:${id}`,
          apertureId, rooms: [roomId, targetRoomId].sort(), curveRoomId: roomId,
          targetRoomId, direction, radial: [...CARDINAL[direction]], width, height,
          depth, overlapDepth, chordDistance, bridgeDistance,
          bottomY: level.baseY + bottom, topY: level.baseY + bottom + height,
          wallCut: { startAngle, endAngle }, wallThreshold, threshold, farThreshold,
          bounds: { ...bounds }, targetFloorBounds: { ...targetBounds },
          floorPatch: { id: `floor-patch:${id}:${apertureId}`, bounds: { ...bounds } },
          targetWallEdgeIds: targetWallEdges.map(wall => wall.id).sort(),
          hostApertureId, targetWallApertureId: hostApertureId,
          revealIds: [`reveal:${id}:${apertureId}:left`, `reveal:${id}:${apertureId}:right`],
          sideReturnIds: [`return:${id}:${apertureId}:left`, `return:${id}:${apertureId}:right`],
          clearanceVolume: { ...bounds, minY: level.baseY, maxY: level.baseY + height },
        });
      }
      return {
        id: apertureId,
        kind: apertureKind, startAngle, endAngle, connects,
        bottom, height, radialThroatId,
      };
    })].sort((a, b) => a.startAngle - b.startAngle || cmp(a.id, b.id));
    const apertureIntervals = kind === 'ring'
      ? apertures.flatMap(aperture => aperture.endAngle <= 360
        ? [{ start: aperture.startAngle, end: aperture.endAngle }]
        : [{ start: aperture.startAngle, end: 360 }, { start: 0, end: aperture.endAngle - 360 }])
        .sort((a, b) => a.start - b.start || a.end - b.end)
      : apertures.map(aperture => ({ start: aperture.startAngle, end: aperture.endAngle }));
    for (let i = 1; i < apertureIntervals.length; ++i)
      if (apertureIntervals[i].start < apertureIntervals[i - 1].end - 1e-9)
        fail(path, 'aperture intervals overlap');
    endpoints = endpoints.map(endpoint => {
      const matches = walls.filter(wall => wall.levelId === levelId && wall.kind !== 'open' &&
        sectionCompatible(section, wall.section) &&
        (comparePoint(wall.from, endpoint.position) === 0 || comparePoint(wall.to, endpoint.position) === 0) &&
        (wall.to[0] - wall.from[0]) * endpoint.tangent[1] ===
          (wall.to[1] - wall.from[1]) * endpoint.tangent[0]);
      if (source.requireGridJoin === true && matches.length === 0)
        fail(path, `${endpoint.end} endpoint has no tangent straight-wall socket`);
      const transitionIds = matches.map(wall => `curve-transition:${id}:${endpoint.end}:${wall.id}`);
      for (let i = 0; i < matches.length; ++i) curveTransitions.push({
        id: transitionIds[i], kind: 'tangent-wall', levelId, curveId: `curve:${id}`,
        curveEnd: endpoint.end, position: [...endpoint.position], tangent: [...endpoint.tangent],
        wallEdgeId: matches[i].id, section: { ...section }, ownerId: `curve:${id}`,
      });
      return {
        ...endpoint,
        socket: {
          id: `curve-socket:${id}:${endpoint.end}`,
          kind: 'tangent-wall',
          levelId,
          section: { ...section },
          compatibleWallEdgeIds: matches.map(wall => wall.id).sort(),
          transitionIds: transitionIds.sort(),
        },
      };
    });
    curves.push({
      id: `curve:${id}`, sourceId: id, levelId, roomId, kind, center, radius,
      clockwise: source.clockwise === true, section, endpoints, apertures,
      radialThroatIds: apertures.map(aperture => aperture.radialThroatId).filter(Boolean).sort(),
    });
  }
  return {
    curves: stableSort(curves, curve => curve.id),
    radialThroats: stableSort(radialThroats, throat => throat.id),
    curveTransitions: stableSort(curveTransitions, transition => transition.id),
  };
}

function rectangle(value, path) {
  if (!value || typeof value !== 'object') fail(path, 'must be an object');
  const rect = {
    x: finite(value.x, `${path}.x`), z: finite(value.z, `${path}.z`),
    width: finite(value.width, `${path}.width`), depth: finite(value.depth, `${path}.depth`),
  };
  if (rect.width <= 0 || rect.depth <= 0) fail(path, 'width and depth must be positive');
  return rect;
}

function beamBounds(beam) {
  const half = Math.max(beam.section[0], beam.section[1]) / 2;
  return {
    minX: Math.min(beam.from[0], beam.to[0]) - half, maxX: Math.max(beam.from[0], beam.to[0]) + half,
    minY: Math.min(beam.from[1], beam.to[1]) - half, maxY: Math.max(beam.from[1], beam.to[1]) + half,
    minZ: Math.min(beam.from[2], beam.to[2]) - half, maxZ: Math.max(beam.from[2], beam.to[2]) + half,
  };
}

function volumesOverlap(a, b) {
  return a.minX < b.maxX - 1e-9 && b.minX < a.maxX - 1e-9 &&
    a.minY < b.maxY - 1e-9 && b.minY < a.maxY - 1e-9 &&
    a.minZ < b.maxZ - 1e-9 && b.minZ < a.maxZ - 1e-9;
}

function buildStairs(plan, levelsById, roomsById, beamMembers) {
  const seenIds = new Set();
  return stableSort((plan.stairs || []).map((source, index) => {
    const path = `stairs[${index}]`;
    const id = string(source.id, `${path}.id`);
    if (seenIds.has(id)) fail(`${path}.id`, `duplicate id ${id}`);
    seenIds.add(id);
    const lowerLevelId = string(source.lowerLevelId, `${path}.lowerLevelId`);
    const upperLevelId = string(source.upperLevelId, `${path}.upperLevelId`);
    const lower = levelsById.get(lowerLevelId), upper = levelsById.get(upperLevelId);
    if (!lower || !upper || upper.baseY <= lower.baseY) fail(path, 'levels must exist and upper must be above lower');
    const lowerRoomId = string(source.lowerRoomId, `${path}.lowerRoomId`);
    const upperRoomId = string(source.upperRoomId, `${path}.upperRoomId`);
    if (roomsById.get(lowerRoomId)?.levelId !== lowerLevelId) fail(`${path}.lowerRoomId`, 'room is not on lower level');
    if (roomsById.get(upperRoomId)?.levelId !== upperLevelId) fail(`${path}.upperRoomId`, 'room is not on upper level');
    if (roomsById.get(lowerRoomId).openToBelow || roomsById.get(upperRoomId).openToBelow)
      fail(path, 'stairs require walkable lower and upper rooms');
    const width = finite(source.width, `${path}.width`);
    const tread = finite(source.tread, `${path}.tread`);
    const maxRiser = finite(source.maxRiser ?? 0.2, `${path}.maxRiser`);
    const headroom = finite(source.headroom, `${path}.headroom`);
    if (width < 1.2) fail(`${path}.width`, 'must be at least 1.2m');
    if (tread < 0.25) fail(`${path}.tread`, 'must be at least 0.25m');
    if (maxRiser <= 0 || maxRiser > 0.2) fail(`${path}.maxRiser`, 'must be in (0, 0.2]m');
    if (headroom < 2.1) fail(`${path}.headroom`, 'must be at least 2.1m');
    const rise = upper.baseY - lower.baseY;
    const stepCount = Math.ceil(rise / maxRiser);
    const riser = rise / stepCount;
    if (!Array.isArray(source.flights) || source.flights.length === 0)
      fail(`${path}.flights`, 'must explicitly declare one or more flights');
    if (!Array.isArray(source.landings) || source.landings.length < 2)
      fail(`${path}.landings`, 'must explicitly declare lower and upper landings');
    const lowerRoom = roomsById.get(lowerRoomId), upperRoom = roomsById.get(upperRoomId);
    let stepCursor = 0;
    const flightIds = new Set();
    const flights = source.flights.map((flight, flightIndex) => {
      const flightPath = `${path}.flights[${flightIndex}]`;
      const flightId = string(flight.id, `${flightPath}.id`);
      if (flightIds.has(flightId)) fail(`${flightPath}.id`, `duplicate id ${flightId}`);
      flightIds.add(flightId);
      const direction = string(flight.direction, `${flightPath}.direction`);
      if (!CARDINAL[direction]) fail(`${flightPath}.direction`, 'must be cardinal');
      const footprint = rectangle(flight.footprint, `${flightPath}.footprint`);
      const flightSteps = integer(flight.stepCount, `${flightPath}.stepCount`);
      if (flightSteps <= 0) fail(`${flightPath}.stepCount`, 'must be positive');
      const run = flightSteps * tread;
      const travelExtent = direction === 'E' || direction === 'W' ? footprint.width : footprint.depth;
      const transverseExtent = direction === 'E' || direction === 'W' ? footprint.depth : footprint.width;
      if (travelExtent + 1e-9 < run) fail(`${flightPath}.footprint`, `needs ${run}m travel extent`);
      if (transverseExtent + 1e-9 < width) fail(`${flightPath}.footprint`, `transverse extent must fit ${width}m stair width`);
      const bounds = boundsFromRect(footprint);
      if (!roomCoversBounds(lowerRoom, bounds) || !roomCoversBounds(upperRoom, bounds))
        fail(`${flightPath}.footprint`, 'must stay inside both named stacked room floors');
      const fromY = lower.baseY + stepCursor * riser;
      stepCursor += flightSteps;
      const toY = lower.baseY + stepCursor * riser;
      return {
        id: `flight:${id}:${flightId}`, sourceId: flightId, index: flightIndex,
        direction, width, stepCount: flightSteps, riser, tread, run, footprint,
        fromY, toY,
        centerline: [
          [footprint.x + footprint.width / 2 - CARDINAL[direction][0] * run / 2,
            fromY, footprint.z + footprint.depth / 2 - CARDINAL[direction][1] * run / 2],
          [footprint.x + footprint.width / 2 + CARDINAL[direction][0] * run / 2,
            toY, footprint.z + footprint.depth / 2 + CARDINAL[direction][1] * run / 2],
        ],
        sweptVolume: { ...bounds, minY: fromY, maxY: toY + headroom },
      };
    });
    if (stepCursor !== stepCount) fail(`${path}.flights`, `step counts total ${stepCursor}, expected ${stepCount}`);
    const landingIds = new Set();
    const landings = source.landings.map((landing, landingIndex) => {
      const landingPath = `${path}.landings[${landingIndex}]`;
      const landingId = string(landing.id, `${landingPath}.id`);
      if (landingIds.has(landingId)) fail(`${landingPath}.id`, `duplicate id ${landingId}`);
      landingIds.add(landingId);
      const kind = string(landing.kind, `${landingPath}.kind`);
      if (!['lower', 'intermediate', 'upper'].includes(kind)) fail(`${landingPath}.kind`, 'must be lower, intermediate, or upper');
      const bounds = rectangle(landing.bounds, `${landingPath}.bounds`);
      if (bounds.width + 1e-9 < width || bounds.depth + 1e-9 < width)
        fail(`${landingPath}.bounds`, `both dimensions must be at least stair width ${width}m`);
      const volumeBounds = boundsFromRect(bounds);
      const targetRoom = kind === 'upper' ? upperRoom : lowerRoom;
      if (!roomCoversBounds(targetRoom, volumeBounds)) fail(`${landingPath}.bounds`, `must stay inside ${targetRoom.id} floor`);
      const elevation = kind === 'upper' ? upper.baseY :
        kind === 'lower' ? lower.baseY : finite(landing.elevation, `${landingPath}.elevation`);
      if (elevation < lower.baseY || elevation > upper.baseY) fail(`${landingPath}.elevation`, 'must lie between stair levels');
      const compiledId = `landing:${id}:${landingId}`;
      return { id: compiledId, sourceId: landingId, kind, bounds, elevation,
        roomId: kind === 'upper' ? upperRoomId : lowerRoomId,
        sweptVolumeId: `swept:${compiledId}` };
    });
    const lowerLandings = landings.filter(landing => landing.kind === 'lower');
    const upperLandings = landings.filter(landing => landing.kind === 'upper');
    if (lowerLandings.length !== 1 || upperLandings.length !== 1)
      fail(`${path}.landings`, 'must contain exactly one lower and one upper landing');
    if (!landingSupportsFlightEnd(lowerLandings[0].bounds, flights[0], false) ||
        !landingSupportsFlightEnd(upperLandings[0].bounds, flights[flights.length - 1], true))
      fail(`${path}.landings`, 'lower/upper landings must cover the full-width directed flight ends');
    const intermediates = landings.filter(landing => landing.kind === 'intermediate');
    if (intermediates.length !== Math.max(0, flights.length - 1))
      fail(`${path}.landings`, 'needs one intermediate landing between consecutive flights');
    for (let i = 0; i < intermediates.length; ++i) {
      if (Math.abs(intermediates[i].elevation - flights[i].toY) > 1e-9 ||
          Math.abs(intermediates[i].elevation - flights[i + 1].fromY) > 1e-9)
        fail(`${path}.landings`, `intermediate landing ${intermediates[i].sourceId} elevation must match adjacent flights`);
      if (!landingSupportsFlightEnd(intermediates[i].bounds, flights[i], true) ||
          !landingSupportsFlightEnd(intermediates[i].bounds, flights[i + 1], false))
        fail(`${path}.landings`, `intermediate landing ${intermediates[i].sourceId} must cover both full-width directed flight ends`);
    }
    const entryDirection = source.entryDirection ?? flights[0].direction;
    const exitDirection = source.exitDirection ?? flights[flights.length - 1].direction;
    if (!CARDINAL[entryDirection] || !CARDINAL[exitDirection])
      fail(path, 'entryDirection and exitDirection must be cardinal');
    if (entryDirection !== flights[0].direction ||
        exitDirection !== flights[flights.length - 1].direction)
      fail(path, 'entry/exit directions must match the first/last flight');
    for (let i = 0; i < flights.length; ++i) {
      flights[i].fromLandingId = i === 0 ? lowerLandings[0].id : intermediates[i - 1].id;
      flights[i].toLandingId = i === flights.length - 1 ? upperLandings[0].id : intermediates[i].id;
    }
    const landingSweptVolumes = landings.map(landing => ({
      id: landing.sweptVolumeId, ...boundsFromRect(landing.bounds),
      minY: landing.elevation, maxY: landing.elevation + headroom,
    }));
    for (const flight of flights)
      for (const beam of beamMembers)
        if (volumesOverlap(flight.sweptVolume, beamBounds(beam)))
          fail(`${path}.flights`, `swept headroom volume intersects beam ${beam.id}`);
    for (const swept of landingSweptVolumes)
      for (const beam of beamMembers)
        if (volumesOverlap(swept, beamBounds(beam)))
          fail(`${path}.landings`, `swept headroom volume intersects beam ${beam.id}`);
    const holeRegions = [...flights.map(flight => flight.footprint),
      ...landings.filter(landing => landing.kind !== 'lower').map(landing => landing.bounds)];
    const holes = holeRegions.map((bounds, holeIndex) => ({
      id: `hole:${id}:${holeIndex}`, levelId: upperLevelId,
      floorId: `floor:${idToken(upperLevelId)}:${idToken(upperRoomId)}`,
      footprint: { ...bounds }, minY: lower.baseY, maxY: upper.baseY + headroom,
    }));
    const holeBounds = {
      x: Math.min(...holeRegions.map(region => region.x)),
      z: Math.min(...holeRegions.map(region => region.z)),
      width: Math.max(...holeRegions.map(region => region.x + region.width)) -
        Math.min(...holeRegions.map(region => region.x)),
      depth: Math.max(...holeRegions.map(region => region.z + region.depth)) -
        Math.min(...holeRegions.map(region => region.z)),
    };
    return {
      id: `stair:${id}`, sourceId: id, lowerLevelId, upperLevelId,
      lowerRoomId, upperRoomId, width, rise, tread, riser, stepCount, headroom,
      entryDirection, exitDirection,
      flights, landings, holes,
      route: {
        waypoints: [
          [lowerLandings[0].bounds.x + lowerLandings[0].bounds.width / 2, lower.baseY,
            lowerLandings[0].bounds.z + lowerLandings[0].bounds.depth / 2],
          ...flights.flatMap(flight => flight.centerline),
          [upperLandings[0].bounds.x + upperLandings[0].bounds.width / 2, upper.baseY,
            upperLandings[0].bounds.z + upperLandings[0].bounds.depth / 2],
        ],
        sweptVolumeIds: [
          ...flights.map(flight => `swept:${flight.id}`),
          ...landingSweptVolumes.map(volume => volume.id),
        ],
      },
      sweptVolumes: [
        ...flights.map(flight => ({ id: `swept:${flight.id}`, ...flight.sweptVolume })),
        ...landingSweptVolumes,
      ],
      voids: [
        { id: `void:${id}:ceiling`, levelId: lowerLevelId, kind: 'ceiling',
          bounds: { ...holeBounds }, regions: holeRegions.map(region => ({ ...region })) },
        { id: `void:${id}:floor`, levelId: upperLevelId, kind: 'floor',
          bounds: { ...holeBounds }, regions: holeRegions.map(region => ({ ...region })) },
      ],
    };
  }), stair => stair.id);
}

function buildBeamMembers(plan, levelsById) {
  const seen = new Set();
  return stableSort((plan.beams || []).map((beam, index) => {
    const path = `beams[${index}]`;
    const levelId = string(beam.levelId, `${path}.levelId`);
    if (!levelsById.has(levelId)) fail(`${path}.levelId`, `unknown level ${levelId}`);
    let a = point3(beam.from, `${path}.from`), b = point3(beam.to, `${path}.to`);
    if (a.every((value, i) => value === b[i])) fail(path, 'member endpoints must differ');
    if (cmp(a.join(','), b.join(',')) > 0) [a, b] = [b, a];
    if (!Array.isArray(beam.section) || beam.section.length !== 2) fail(`${path}.section`, 'must be [width,height]');
    const section = beam.section.map((value, i) => finite(value, `${path}.section[${i}]`));
    if (section.some(value => value <= 0)) fail(`${path}.section`, 'dimensions must be positive');
    const semantic = `${levelId}:${a.join(',')}|${b.join(',')}`;
    if (seen.has(semantic)) fail(path, 'duplicates a beam graph member');
    seen.add(semantic);
    return {
      id: `beam:${idToken(levelId)}:${a.join(',')}|${b.join(',')}:${idToken(beam.role ?? 'beam')}`,
      levelId, from: a, to: b, section,
      jointFamily: beam.jointFamily ?? 'mortise-tenon',
      role: beam.role ?? 'beam', material: beam.material ?? 'castle.oak',
      bearing: beam.bearing ?? null,
      joist: beam.joist ?? (beam.role === 'joist' ? { spacing: beam.spacing ?? null } : null),
    };
  }), beam => beam.id);
}

function flatRecordList(source, levelsById, name) {
  const records = (source || []).map((item, index) => {
    const path = `${name}[${index}]`;
    const id = string(item.id, `${path}.id`);
    const levelId = string(item.levelId, `${path}.levelId`);
    if (!levelsById.has(levelId)) fail(`${path}.levelId`, `unknown level ${levelId}`);
    return { ...item, id: `${name.slice(0, -1)}:${id}`, sourceId: id, levelId };
  });
  validateUniqueIds(records, name);
  return stableSort(records, item => item.id);
}

function addGraphEdge(edges, edge) {
  const rooms = [...edge.rooms].sort();
  const id = `route:${edge.kind}:${idToken(edge.sourceId)}:${rooms.map(idToken).join('|')}`;
  if (!edges.some(existing => existing.id === id)) edges.push({ id, ...edge, rooms });
}

function buildPortals(levelsById, roomsById, walls, wallModules, radialThroats) {
  const groups = new Map();
  for (const wall of walls)
    for (const opening of wall.openings)
      if (PORTAL_KINDS.has(opening.kind) && wall.connects &&
          opening.claimedByRadialThroatId === undefined) {
        if (!groups.has(opening.apertureId)) groups.set(opening.apertureId,
          { wall, opening, hostEdgeIds: [] });
        groups.get(opening.apertureId).hostEdgeIds.push(wall.id);
      }
  const portals = [];
  for (const [apertureId, group] of groups) {
    const { wall, opening } = group;
    const level = levelsById.get(wall.levelId);
    const alongX = opening.segmentFrom[1] === opening.segmentTo[1];
    const half = wall.section.thickness / 2;
    const bounds = alongX ? {
      minX: opening.segmentFrom[0] + opening.globalStart,
      maxX: opening.segmentFrom[0] + opening.globalEnd,
      minZ: opening.segmentFrom[1] - half,
      maxZ: opening.segmentFrom[1] + half,
    } : {
      minX: opening.segmentFrom[0] - half,
      maxX: opening.segmentFrom[0] + half,
      minZ: opening.segmentFrom[1] + opening.globalStart,
      maxZ: opening.segmentFrom[1] + opening.globalEnd,
    };
    const thresholds = alongX
      ? [[(bounds.minX + bounds.maxX) / 2, level.baseY, bounds.minZ],
        [(bounds.minX + bounds.maxX) / 2, level.baseY, bounds.maxZ]]
      : [[bounds.minX, level.baseY, (bounds.minZ + bounds.maxZ) / 2],
        [bounds.maxX, level.baseY, (bounds.minZ + bounds.maxZ) / 2]];
    const roomThresholds = {};
    for (const roomId of wall.connects.filter(value => value !== 'outside')) {
      const room = roomsById.get(roomId);
      if (room.openToBelow) fail(`portal.${apertureId}`, `room ${roomId} has no walkable floor`);
      roomThresholds[roomId] = thresholds.find(point => pointInsideRoom(room, point)) ?? null;
      if (!roomThresholds[roomId]) fail(`portal.${apertureId}`, `has no threshold inside room ${roomId}`);
    }
    portals.push({
      id: `portal:${apertureId}`, kind: opening.kind,
      sourceId: wall.overrideId, levelId: wall.levelId,
      rooms: [...wall.connects].sort(),
      floorIds: wall.connects.filter(roomId => roomId !== 'outside')
        .map(roomId => `floor:${idToken(wall.levelId)}:${idToken(roomId)}`).sort(),
      clearWidth: opening.globalEnd - opening.globalStart,
      clearHeight: opening.top - opening.bottom,
      bounds: { ...bounds, minY: level.baseY + opening.bottom, maxY: level.baseY + opening.top },
      thresholds, roomThresholds,
      hostEdgeIds: group.hostEdgeIds.sort(),
      hostModuleIds: wallModules.filter(module => module.sourceEdgeIds
        .some(edgeId => group.hostEdgeIds.includes(edgeId))).map(module => module.id).sort(),
      sweptVolumeId: `route-volume:${apertureId}`,
    });
  }
  for (const throat of radialThroats) {
    const targetModuleIds = wallModules.filter(module => module.sourceEdgeIds
      .some(edgeId => throat.targetWallEdgeIds.includes(edgeId))).map(module => module.id).sort();
    portals.push({
      id: `portal:${throat.id}`, kind: throat.kind, sourceId: throat.apertureId,
      levelId: throat.levelId, rooms: [...throat.rooms],
      floorIds: throat.rooms.map(roomId =>
        `floor:${idToken(throat.levelId)}:${idToken(roomId)}`).sort(),
      clearWidth: throat.width, clearHeight: throat.height,
      bounds: { ...throat.clearanceVolume },
      thresholds: [
        [throat.threshold[0], throat.bottomY, throat.threshold[1]],
        [throat.farThreshold[0], throat.bottomY, throat.farThreshold[1]],
      ],
      roomThresholds: {
        [throat.curveRoomId]: [throat.threshold[0], throat.bottomY, throat.threshold[1]],
        [throat.targetRoomId]: [throat.farThreshold[0], throat.bottomY, throat.farThreshold[1]],
      },
      hostEdgeIds: [throat.curveId, ...throat.targetWallEdgeIds].sort(),
      hostModuleIds: targetModuleIds,
      claimedApertureId: throat.hostApertureId,
      sweptVolumeId: `route-volume:${throat.id}`,
    });
  }
  return stableSort(portals, portal => portal.id);
}

function buildOpenBoundaries(walls) {
  const groups = new Map();
  for (const wall of walls.filter(candidate => candidate.kind === 'open')) {
    const sourceId = wall.overrideId ?? wall.id;
    const key = `${wall.levelId}:${sourceId}`;
    if (!groups.has(key)) groups.set(key, { sourceId, edges: [] });
    groups.get(key).edges.push(wall);
  }
  return stableSort([...groups.values()].map(({ sourceId, edges }) => {
    const connects = edges[0].connects;
    const claimedByRadialThroatIds = [...new Set(edges.flatMap(edge => edge.openings
      .map(opening => opening.claimedByRadialThroatId).filter(Boolean)))].sort();
    const kind = claimedByRadialThroatIds.length ? 'radial-throat-host' :
      connects?.includes('outside') ? 'exterior-entry' : connects ? 'room-open' : 'void-edge';
    const railRequired = kind === 'void-edge';
    return {
      id: `open-boundary:${idToken(edges[0].levelId)}:${idToken(sourceId)}`, sourceId,
      levelId: edges[0].levelId, kind,
      roomIds: [...new Set(edges.flatMap(edge => edge.roomIds))].sort(),
      hostEdgeIds: edges.map(edge => edge.id).sort(),
      claimedByRadialThroatIds,
      circulationSuppressed: claimedByRadialThroatIds.length > 0,
      clearWidth: edges.length,
      clearHeight: edges[0].section.height,
      rail: {
        required: railRequired,
        profile: edges[0].railProfile ?? (railRequired ? 'castle.guardrail' : null),
      },
    };
  }), boundary => boundary.id);
}

function buildRoomGraph(plan, rooms, portals, stairs, beamMembers, fixtures, floors) {
  const stairsById = new Map(stairs.map(stair => [stair.id, stair]));
  const nodes = rooms.map(room => ({
    id: room.id, levelId: room.levelId, use: room.use,
    required: room.required !== false, walkable: !room.openToBelow,
  })).sort((a, b) => cmp(a.id, b.id));
  const edges = [];
  for (const portal of portals) {
    for (const beam of beamMembers)
      if (volumesOverlap(portal.bounds, beamBounds(beam)))
        fail(`portal.${portal.id}`, `swept headroom intersects beam ${beam.id}`);
    for (const fixture of fixtures)
      if (fixture.clearance && volumesOverlap(portal.bounds, fixture.clearance))
        fail(`portal.${portal.id}`, `swept clearance intersects fixture ${fixture.id}`);
    addGraphEdge(edges, {
      kind: portal.kind, sourceId: portal.sourceId, rooms: portal.rooms,
      portalVolumeId: portal.id, floorIds: portal.floorIds,
      thresholds: portal.thresholds, roomThresholds: portal.roomThresholds,
      clearWidth: portal.clearWidth, sweptVolumeIds: [portal.sweptVolumeId],
    });
  }
  for (const stair of stairs)
    addGraphEdge(edges, {
      kind: 'stair', sourceId: stair.sourceId,
      rooms: [stair.lowerRoomId, stair.upperRoomId],
      portalVolumeId: stair.id,
      floorIds: [
        `floor:${idToken(stair.lowerLevelId)}:${idToken(stair.lowerRoomId)}`,
        `floor:${idToken(stair.upperLevelId)}:${idToken(stair.upperRoomId)}`,
      ],
      thresholds: [stair.route.waypoints[0],
        stair.route.waypoints[stair.route.waypoints.length - 1]],
      roomThresholds: {
        [stair.lowerRoomId]: stair.route.waypoints[0],
        [stair.upperRoomId]: stair.route.waypoints[stair.route.waypoints.length - 1],
      },
      clearWidth: stair.width,
      sweptVolumeIds: stair.route.sweptVolumeIds,
    });
  edges.sort((a, b) => cmp(a.id, b.id));
  const entryRoomId = string(plan.entryRoomId, 'entryRoomId');
  if (!nodes.some(node => node.id === entryRoomId)) fail('entryRoomId', `unknown room ${entryRoomId}`);
  if (!nodes.find(node => node.id === entryRoomId).walkable)
    fail('entryRoomId', 'must name a walkable room');
  const entryPortal = portals.find(portal =>
    portal.rooms.includes('outside') && portal.rooms.includes(entryRoomId)) ?? null;
  const adjacency = new Map(nodes.map(node => [node.id, []]));
  for (const edge of edges) {
    const realRooms = edge.rooms.filter(roomId => roomId !== 'outside');
    if (realRooms.length === 1 && edge.rooms.includes('outside')) adjacency.get(realRooms[0]).push('outside');
    if (realRooms.length === 2) {
      adjacency.get(realRooms[0]).push(realRooms[1]);
      adjacency.get(realRooms[1]).push(realRooms[0]);
    }
  }
  const reachable = new Set([entryRoomId]);
  const queue = [entryRoomId], parent = new Map(), parentEdge = new Map();
  while (queue.length) {
    const current = queue.shift();
    for (const next of [...(adjacency.get(current) || [])].sort()) {
      if (next === 'outside' || reachable.has(next)) continue;
      reachable.add(next);
      parent.set(next, current);
      parentEdge.set(next, edges.find(edge =>
        edge.rooms.includes(current) && edge.rooms.includes(next)));
      queue.push(next);
    }
  }
  const missing = nodes.filter(node => node.required && !reachable.has(node.id)).map(node => node.id);
  if (missing.length) fail('roomGraph', `required rooms are unreachable from ${entryRoomId}: ${missing.join(', ')}`);
  const routeObstaclesForRoom = (roomId, floor) => [
    ...floor.holes,
    ...stairs.filter(stair => stair.lowerRoomId === roomId).flatMap(stair => [
      ...stair.flights.map(flight => ({
        id: `route-obstacle:${flight.id}`, footprint: flight.footprint,
        replacementLandingId: null,
      })),
      ...stair.landings.filter(landing => landing.kind !== 'lower' &&
        landing.elevation < floor.elevation + MIN_PORTAL_HEIGHT - 1e-9).map(landing => ({
        id: `route-obstacle:${landing.id}`, footprint: landing.bounds,
        replacementLandingId: null,
      })),
    ]),
  ];
  const walkRoute = [...reachable].sort().map(roomId => {
    const path = [], routeEdges = [];
    for (let cursor = roomId; cursor !== undefined; cursor = parent.get(cursor)) {
      path.push(cursor);
      if (parentEdge.has(cursor)) routeEdges.push(parentEdge.get(cursor));
    }
    routeEdges.reverse();
    const entryEdge = entryPortal ? edges.find(edge => edge.portalVolumeId === entryPortal.id) : null;
    const fullEdges = entryEdge && !routeEdges.some(edge => edge.id === entryEdge.id)
      ? [entryEdge, ...routeEdges] : routeEdges;
    const fromEntry = [...path].reverse();
    const roomSegments = [];
    for (let i = 0; i + 1 < fullEdges.length; ++i) {
      const sharedRoomId = fullEdges[i].rooms.find(candidate =>
        candidate !== 'outside' && fullEdges[i + 1].rooms.includes(candidate));
      if (!sharedRoomId) fail('walkRoute', `connectors ${fullEdges[i].id} and ${fullEdges[i + 1].id} share no room`);
      const floor = floors.find(candidate => candidate.roomId === sharedRoomId);
      if (!floor) fail('walkRoute', `room ${sharedRoomId} has no walkable floor`);
      const segment = validateRoomSegment(rooms.find(room => room.id === sharedRoomId),
        fullEdges[i].roomThresholds[sharedRoomId],
        fullEdges[i + 1].roomThresholds[sharedRoomId],
        MIN_PORTAL_WIDTH, routeObstaclesForRoom(sharedRoomId, floor),
        `walkRoute.${roomId}`);
      segment.id = `route-segment:${roomId}:${i}`;
      for (const beam of beamMembers)
        if (segment.segments.some(leg => volumesOverlap(leg.bounds, beamBounds(beam))))
          fail(`walkRoute.${roomId}`, `room swept headroom intersects beam ${beam.id}`);
      for (const fixture of fixtures)
        if (fixture.clearance && segment.segments.some(leg => volumesOverlap(leg.bounds, fixture.clearance)))
          fail(`walkRoute.${roomId}`, `room swept clearance intersects fixture ${fixture.id}`);
      roomSegments.push(segment);
    }
    const traversalPairs = fullEdges.map((edge, index) => entryEdge
      ? { fromRoomId: index === 0 ? 'outside' : fromEntry[index - 1], toRoomId: fromEntry[index] }
      : { fromRoomId: fromEntry[index], toRoomId: fromEntry[index + 1] });
    const thresholdFor = (edge, roomId) => {
      if (roomId !== 'outside') return edge.roomThresholds[roomId];
      const roomPoints = Object.values(edge.roomThresholds);
      return edge.thresholds.find(point => !roomPoints.some(roomPoint =>
        point.every((value, coordinate) => Math.abs(value - roomPoint[coordinate]) < 1e-9))) ?? edge.thresholds[0];
    };
    const traversals = fullEdges.map((edge, index) => ({
      edgeId: edge.id, fromRoomId: traversalPairs[index].fromRoomId,
      toRoomId: traversalPairs[index].toRoomId,
      from: [...thresholdFor(edge, traversalPairs[index].fromRoomId)],
      to: [...thresholdFor(edge, traversalPairs[index].toRoomId)],
    }));
    const traversalWaypoints = (edge, traversal) => {
      if (edge.kind !== 'stair') return [traversal.from, traversal.to];
      const stair = stairsById.get(edge.portalVolumeId);
      if (!stair) fail(`walkRoute.${roomId}`, `stair edge ${edge.id} has no stair record`);
      const forward = traversal.fromRoomId === stair.lowerRoomId &&
        traversal.toRoomId === stair.upperRoomId;
      const reverse = traversal.fromRoomId === stair.upperRoomId &&
        traversal.toRoomId === stair.lowerRoomId;
      if (!forward && !reverse)
        fail(`walkRoute.${roomId}`, `stair ${stair.id} traversal rooms do not match its endpoints`);
      return (forward ? stair.route.waypoints : [...stair.route.waypoints].reverse())
        .map(point => [...point]);
    };
    const waypoints = [];
    if (traversals.length) {
      waypoints.push([...traversals[0].from]);
      for (let i = 0; i < traversals.length; ++i) {
        waypoints.push(...traversalWaypoints(fullEdges[i], traversals[i]).slice(1));
        if (roomSegments[i])
          waypoints.push(...roomSegments[i].waypoints.slice(1).map(point => [...point]));
      }
    }
    return {
      roomId, fromEntry,
      edgeIds: fullEdges.map(edge => edge.id),
      sweptVolumeIds: [
        ...fullEdges.flatMap(edge => edge.sweptVolumeIds || []),
        ...roomSegments.map(segment => segment.id),
      ],
      waypoints, traversals, roomSegments,
    };
  });
  return {
    entryRoomId, entryPortalId: entryPortal?.id ?? null,
    nodes, edges, reachableRoomIds: [...reachable].sort(), walkRoute,
  };
}

function collectOccupiedVolumes(levels, stairs, fixtures, radialThroats, portals, floors) {
  const volumes = [];
  for (const level of levels)
    for (const room of level.rooms) {
      if (room.openToBelow) continue;
      if (room.boundary?.kind === 'circle') {
        const floor = floors.find(candidate => candidate.levelId === level.id && candidate.roomId === room.id);
        volumes.push({
          id: `occupied:room:${idToken(room.id)}`, kind: 'room-circle', levelId: level.id,
          roomId: room.id, center: [...room.boundary.center], radius: floor.regions[0].radius,
          minY: level.baseY, maxY: level.baseY + level.height,
        });
      } else for (const [x, z] of room._cells) volumes.push({
        id: `occupied:room:${idToken(room.id)}:${x},${z}`, kind: 'room-cell', levelId: level.id,
        roomId: room.id, bounds: { minX: x, minY: level.baseY, minZ: z, maxX: x + 1, maxY: level.baseY + level.height, maxZ: z + 1 },
      });
    }
  for (const stair of stairs)
    for (const swept of stair.sweptVolumes) volumes.push({
      id: `occupied:${swept.id}`, kind: 'stair-clearance', levelId: stair.lowerLevelId,
      bounds: {
        minX: swept.minX, maxX: swept.maxX, minY: swept.minY,
        maxY: swept.maxY, minZ: swept.minZ, maxZ: swept.maxZ,
      },
    });
  for (const throat of radialThroats) volumes.push({
    id: `occupied:${throat.id}`, kind: 'radial-throat-clearance', levelId: throat.levelId,
    bounds: { ...throat.clearanceVolume },
  });
  for (const portal of portals) volumes.push({
    id: portal.sweptVolumeId, kind: 'portal-clearance', levelId: portal.levelId,
    bounds: { ...portal.bounds },
  });
  for (const fixture of fixtures) if (fixture.clearance) volumes.push({
    id: `occupied:${fixture.id}`, kind: 'fixture-clearance', levelId: fixture.levelId,
    bounds: { ...fixture.clearance },
  });
  return stableSort(volumes, volume => volume.id);
}

function validateUniqueIds(values, path) {
  const seen = new Set();
  for (const value of values) {
    if (seen.has(value.id)) fail(path, `duplicate id ${value.id}`);
    seen.add(value.id);
  }
}

function validateCircularGeometry(levels) {
  for (const level of levels) {
    const circles = level.rooms.filter(room => room.boundary?.kind === 'circle');
    const gridRooms = level.rooms.filter(room => !room.boundary);
    for (const circle of circles) {
      const [cx, cz] = circle.boundary.center, radius2 = circle.boundary.radius ** 2;
      for (const room of gridRooms)
        for (const [x, z] of room._cells) {
          const nearestX = Math.max(x, Math.min(cx, x + 1));
          const nearestZ = Math.max(z, Math.min(cz, z + 1));
          if ((nearestX - cx) ** 2 + (nearestZ - cz) ** 2 < radius2 - 1e-9)
            fail(`levels.${level.id}.rooms.${circle.id}`,
              `circle overlaps cell ${x},${z} of room ${room.id}`);
        }
    }
    for (let i = 0; i < circles.length; ++i)
      for (let j = i + 1; j < circles.length; ++j) {
        const a = circles[i], b = circles[j];
        const distance = Math.hypot(a.boundary.center[0] - b.boundary.center[0],
          a.boundary.center[1] - b.boundary.center[1]);
        if (distance < a.boundary.radius + b.boundary.radius - 1e-9)
          fail(`levels.${level.id}.rooms`, `circular rooms ${a.id} and ${b.id} overlap`);
      }
  }
}

function normalizeFloorStructure(room, level, walls) {
  const source = room.floorStructure || {};
  const direction = source.joistDirection ?? null;
  if (direction !== null && direction !== 'x' && direction !== 'z')
    fail(`rooms.${room.id}.floorStructure.joistDirection`, 'must be x or z');
  const bearingEdgeIds = source.bearingEdgeIds === undefined
    ? walls.filter(wall => wall.roomIds.includes(room.id) && wall.kind !== 'open')
      .map(wall => wall.id).sort()
    : source.bearingEdgeIds.map((id, index) =>
      string(id, `rooms.${room.id}.floorStructure.bearingEdgeIds[${index}]`)).sort();
  for (const id of bearingEdgeIds)
    if (!walls.some(wall => wall.id === id && wall.levelId === level.id && wall.roomIds.includes(room.id)))
      fail(`rooms.${room.id}.floorStructure.bearingEdgeIds`, `unknown bearing edge ${id}`);
  const spacing = source.joistSpacing === undefined ? null :
    finite(source.joistSpacing, `rooms.${room.id}.floorStructure.joistSpacing`);
  if (spacing !== null && spacing <= 0) fail(`rooms.${room.id}.floorStructure.joistSpacing`, 'must be positive');
  return {
    bearing: {
      edgeIds: bearingEdgeIds,
      intermediateSupportIds: [...(source.intermediateSupportIds || [])].sort(),
    },
    joists: { direction, spacing, memberIds: [...(source.joistMemberIds || [])].sort() },
  };
}

function buildVerticalVoids(plan, levelsById, roomsById, stairs) {
  const records = [];
  for (const stair of stairs) records.push({
    id: `vertical-void:${stair.sourceId}`, kind: 'stair',
    lowerLevelId: stair.lowerLevelId, upperLevelId: stair.upperLevelId,
    lowerY: levelsById.get(stair.lowerLevelId).baseY,
    upperY: levelsById.get(stair.upperLevelId).baseY + stair.headroom,
    footprints: stair.holes.map(hole => ({ ...hole.footprint })),
    penetratedFloorIds: [...new Set(stair.holes.map(hole => hole.floorId))].sort(),
    penetratedCeilingIds: [`ceiling:${stair.lowerLevelId}:${stair.lowerRoomId}`],
    lowerRoomIds: [stair.lowerRoomId], upperRoomIds: [stair.upperRoomId],
    openToBelowRoomIds: [],
    railProfile: 'castle.stair-guardrail',
  });
  for (const [index, source] of (plan.verticalVoids || []).entries()) {
    const path = `verticalVoids[${index}]`;
    const id = string(source.id, `${path}.id`);
    const kind = source.kind ?? 'double-height';
    if (!['double-height', 'shaft'].includes(kind)) fail(`${path}.kind`, 'must be double-height or shaft');
    const lowerLevelId = string(source.lowerLevelId, `${path}.lowerLevelId`);
    const upperLevelId = string(source.upperLevelId, `${path}.upperLevelId`);
    const lower = levelsById.get(lowerLevelId), upper = levelsById.get(upperLevelId);
    if (!lower || !upper || upper.baseY <= lower.baseY) fail(path, 'must span ordered existing levels');
    const footprint = rectangle(source.footprint, `${path}.footprint`);
    const roomIds = (source.roomIds || []).map((roomId, roomIndex) =>
      string(roomId, `${path}.roomIds[${roomIndex}]`)).sort();
    for (const roomId of roomIds) if (!roomsById.has(roomId)) fail(`${path}.roomIds`, `unknown room ${roomId}`);
    const requestedLowerRoomId = source.lowerRoomId === undefined ? null :
      string(source.lowerRoomId, `${path}.lowerRoomId`);
    const requestedUpperRoomIds = (source.upperRoomIds || []).map((roomId, roomIndex) =>
      string(roomId, `${path}.upperRoomIds[${roomIndex}]`));
    const referencedIds = [...roomIds, ...requestedUpperRoomIds,
      ...(requestedLowerRoomId ? [requestedLowerRoomId] : [])];
    for (const roomId of referencedIds)
      if (!roomsById.has(roomId)) fail(path, `unknown room ${roomId}`);
    const lowerRoomIds = [...new Set([
      ...roomIds.filter(roomId => roomsById.get(roomId).levelId === lowerLevelId),
      ...(requestedLowerRoomId ? [requestedLowerRoomId] : []),
    ])].sort();
    if (lowerRoomIds.length === 0)
      fail(path, 'must identify lower ceiling ownership with lowerRoomId or a lower-level roomIds entry');
    if (lowerRoomIds.some(roomId => roomsById.get(roomId).levelId !== lowerLevelId))
      fail(`${path}.lowerRoomId`, 'must belong to lowerLevelId');
    if (!roomsCoverBounds(lowerRoomIds.map(roomId => roomsById.get(roomId)), boundsFromRect(footprint)))
      fail(`${path}.lowerRoomId`, 'void footprint must be covered by its lower ceiling-owner rooms');
    const upperRoomIds = [...new Set([
      ...roomIds.filter(roomId => roomsById.get(roomId).levelId === upperLevelId),
      ...requestedUpperRoomIds,
    ])];
    if (upperRoomIds.some(roomId => roomsById.get(roomId).levelId !== upperLevelId))
      fail(`${path}.upperRoomIds`, 'must contain only rooms on upperLevelId');
    for (const roomId of roomIds)
      if (!lowerRoomIds.includes(roomId) && !upperRoomIds.includes(roomId))
        fail(`${path}.roomIds`, `room ${roomId} is on neither endpoint level`);
    for (const room of roomsById.values()) {
      if (!room.openToBelow || room.levelId !== upperLevelId ||
          !lowerRoomIds.includes(room.lowerRoomId)) continue;
      const overlaps = room._cells.some(([x, z]) =>
        x < footprint.x + footprint.width && x + 1 > footprint.x &&
        z < footprint.z + footprint.depth && z + 1 > footprint.z);
      if (overlaps) {
        const fullyCovered = room._cells.every(([x, z]) =>
          x >= footprint.x - 1e-9 && x + 1 <= footprint.x + footprint.width + 1e-9 &&
          z >= footprint.z - 1e-9 && z + 1 <= footprint.z + footprint.depth + 1e-9);
        if (!fullyCovered)
          fail(path, `void footprint must cover the complete openToBelow room ${room.id}`);
        if (!upperRoomIds.includes(room.id)) upperRoomIds.push(room.id);
      }
    }
    upperRoomIds.sort();
    for (const roomId of upperRoomIds) {
      const room = roomsById.get(roomId);
      if (room.openToBelow) {
        if (!lowerRoomIds.includes(room.lowerRoomId))
          fail(`${path}.upperRoomIds`, `air room ${roomId} belongs to another lower room`);
        if (!room._cells.every(([x, z]) =>
          x >= footprint.x - 1e-9 && x + 1 <= footprint.x + footprint.width + 1e-9 &&
          z >= footprint.z - 1e-9 && z + 1 <= footprint.z + footprint.depth + 1e-9))
          fail(path, `void footprint must cover the complete openToBelow room ${room.id}`);
      } else if (!roomCoversBounds(room, boundsFromRect(footprint)))
        fail(`${path}.upperRoomIds`, `void footprint is not covered by upper room ${roomId}`);
    }
    const openToBelowRoomIds = upperRoomIds.filter(roomId => roomsById.get(roomId).openToBelow);
    records.push({
      id: `vertical-void:${id}`, kind, lowerLevelId, upperLevelId,
      lowerY: lower.baseY, upperY: upper.baseY + upper.height,
      footprints: [footprint],
      roomIds: [...new Set([...lowerRoomIds, ...upperRoomIds])].sort(),
      lowerRoomIds, upperRoomIds, openToBelowRoomIds,
      penetratedFloorIds: upperRoomIds.filter(roomId => !roomsById.get(roomId).openToBelow)
        .map(roomId => `floor:${idToken(upperLevelId)}:${idToken(roomId)}`).sort(),
      penetratedCeilingIds: lowerRoomIds
        .map(roomId => `ceiling:${idToken(lowerLevelId)}:${idToken(roomId)}`).sort(),
      railProfile: source.railProfile ?? 'castle.guardrail',
    });
  }
  const claimedAirRooms = new Set(records.flatMap(record => record.openToBelowRoomIds || []));
  for (const room of [...roomsById.values()].filter(candidate => candidate.openToBelow &&
    !claimedAirRooms.has(candidate.id)).sort((a, b) => cmp(a.id, b.id))) {
    const lowerRoom = roomsById.get(room.lowerRoomId);
    const lowerLevel = levelsById.get(lowerRoom.levelId), upperLevel = levelsById.get(room.levelId);
    records.push({
      id: `vertical-void:open-to-below:${idToken(room.id)}`, kind: 'double-height',
      lowerLevelId: lowerRoom.levelId, upperLevelId: room.levelId,
      lowerY: lowerLevel.baseY, upperY: upperLevel.baseY + upperLevel.height,
      footprints: room._cells.map(([x, z]) => ({ x, z, width: 1, depth: 1 })),
      roomIds: [lowerRoom.id, room.id].sort(), lowerRoomIds: [lowerRoom.id],
      upperRoomIds: [room.id], openToBelowRoomIds: [room.id],
      penetratedFloorIds: [],
      penetratedCeilingIds: [`ceiling:${idToken(lowerRoom.levelId)}:${idToken(lowerRoom.id)}`],
      railProfile: room.railProfile ?? 'castle.guardrail',
    });
  }
  validateUniqueIds(records, 'verticalVoids');
  return stableSort(records, record => record.id);
}

function compile(plan) {
  if (!plan || typeof plan !== 'object' || Array.isArray(plan)) fail('', 'must be an object');
  if (plan.schema !== undefined && plan.schema !== CASTLE_PLAN_SCHEMA)
    fail('schema', `must be ${CASTLE_PLAN_SCHEMA}`);
  const planId = string(plan.id, 'id');
  const seed = integer(plan.seed ?? 0, 'seed');
  const style = plan.style && typeof plan.style === 'object' ? { ...plan.style } : {};
  if (!Array.isArray(plan.levels) || plan.levels.length === 0) fail('levels', 'must be a non-empty array');
  const levels = plan.levels.map((source, levelIndex) => {
    const path = `levels[${levelIndex}]`;
    const id = string(source.id, `${path}.id`);
    const baseY = finite(source.baseY, `${path}.baseY`);
    const height = finite(source.height, `${path}.height`);
    if (height < 2 || height > 6) fail(`${path}.height`, 'must be in [2,6]m');
    if (!Array.isArray(source.rooms) || source.rooms.length === 0) fail(`${path}.rooms`, 'must be non-empty');
    const rooms = source.rooms.map((room, roomIndex) => {
      const roomPath = `${path}.rooms[${roomIndex}]`;
      const id = string(room.id, `${roomPath}.id`);
      const use = string(room.use, `${roomPath}.use`);
      if (room.openToBelow !== undefined && typeof room.openToBelow !== 'boolean')
        fail(`${roomPath}.openToBelow`, 'must be boolean');
      if (room.openToBelow === true) {
        if (use !== 'void') fail(`${roomPath}.use`, 'openToBelow rooms must use void');
        if (room.required !== false) fail(`${roomPath}.required`, 'openToBelow rooms must set required:false');
        string(room.lowerRoomId, `${roomPath}.lowerRoomId`);
      }
      return { ...room, id, use, levelId: source.id, openToBelow: room.openToBelow === true,
        _cells: roomCells(room, roomPath) };
    });
    validateUniqueIds(rooms, `${path}.rooms`);
    return { ...source, id, baseY, height, rooms: stableSort(rooms, room => room.id) };
  });
  validateUniqueIds(levels, 'levels');
  levels.sort((a, b) => a.baseY - b.baseY || cmp(a.id, b.id));
  const levelsById = new Map(levels.map(level => [level.id, level]));
  const allRooms = levels.flatMap(level => level.rooms);
  validateUniqueIds(allRooms, 'rooms');
  const roomsById = new Map(allRooms.map(room => [room.id, room]));
  for (const room of allRooms.filter(candidate => candidate.openToBelow)) {
    const lowerRoom = roomsById.get(room.lowerRoomId);
    const level = levelsById.get(room.levelId);
    if (!lowerRoom || levelsById.get(lowerRoom.levelId).baseY >= level.baseY)
      fail(`rooms.${room.id}.lowerRoomId`, 'must name a room on a lower level');
    if (room.boundary?.kind === 'circle')
      fail(`rooms.${room.id}.openToBelow`, 'circular air rooms are not supported; use a connected cell union');
    for (const [x, z] of room._cells)
      if (!roomCoversBounds(lowerRoom, { minX: x, maxX: x + 1, minZ: z, maxZ: z + 1 }))
        fail(`rooms.${room.id}.lowerRoomId`, `air cell ${x},${z} is not covered by lower room ${lowerRoom.id}`);
  }
  validateCircularGeometry(levels);
  const walls = [], junctions = [], floors = [];
  for (const level of levels) {
    const built = buildWalls(level, level.rooms, style);
    const levelJunctions = addJunctions(built.walls, level);
    walls.push(...built.walls);
    junctions.push(...levelJunctions);
    for (const room of level.rooms) {
      if (room.openToBelow) continue;
      const boundary = roomBoundary(room);
      const floorRegion = boundary.kind === 'circle' ? {
        ...boundary,
        radius: boundary.radius - finite(style.wallThickness ?? 0.6,
          `rooms.${room.id}.wallThickness`) / 2,
      } : boundary;
      if (floorRegion.kind === 'circle' && floorRegion.radius <= 0)
        fail(`rooms.${room.id}.boundary.radius`, 'must exceed half the wall thickness');
      const structure = normalizeFloorStructure(room, level, built.walls);
      const thickness = finite(room.floorThickness ?? 0.25, `rooms.${room.id}.floorThickness`);
      if (thickness <= 0) fail(`rooms.${room.id}.floorThickness`, 'must be positive');
      floors.push({
        id: `floor:${idToken(level.id)}:${idToken(room.id)}`,
        levelId: level.id, roomId: room.id, elevation: level.baseY,
        thickness, floorType: room.floorType ?? 'stone',
        openToSky: room.use === 'court', boundary,
        regions: [floorRegion], extensions: [], holes: [],
        walkableVolumeIds: boundary.kind === 'circle'
          ? [`occupied:room:${idToken(room.id)}`]
          : boundary.cells.map(cell =>
            `occupied:room:${idToken(room.id)}:${cell[0]},${cell[1]}`),
        bearing: structure.bearing, joists: structure.joists,
        openBoundaryIds: [],
      });
    }
  }
  walls.sort((a, b) => cmp(a.id, b.id)); junctions.sort((a, b) => cmp(a.id, b.id));
  for (const wall of walls)
    for (const opening of wall.openings)
      if (PORTAL_KINDS.has(opening.kind) &&
          (opening.localStart < wall.trim.start - 1e-9 ||
           opening.localEnd > 1 - wall.trim.end + 1e-9))
        fail(`levels.${wall.levelId}.edgeOverrides.${wall.overrideId}`,
          `portal ${opening.apertureId} intersects junction trim volume`);
  const curveBuild = buildCurves(plan, levelsById, roomsById, walls);
  const { curves, radialThroats, curveTransitions } = curveBuild;
  for (const throat of radialThroats)
    for (const wallId of throat.targetWallEdgeIds) {
      const wall = walls.find(candidate => candidate.id === wallId);
      wall.radialThroatId = throat.id;
      for (const opening of wall.openings)
        if (opening.apertureId === throat.hostApertureId)
          opening.claimedByRadialThroatId = throat.id;
    }
  for (const room of allRooms.filter(candidate => candidate.boundary?.kind === 'circle')) {
    const boundaryCurve = curves.find(curve => curve.levelId === room.levelId &&
      curve.roomId === room.id && curve.kind === 'ring' &&
      comparePoint(curve.center, room.boundary.center) === 0 &&
      curve.radius === room.boundary.radius);
    if (!boundaryCurve) fail(`rooms.${room.id}.boundary`, 'requires a matching ring curve');
    if (!boundaryCurve.radialThroatIds.length)
      fail(`rooms.${room.id}.boundary`,
        'requires a curve door/arch/open portal connection with a finite radial throat');
    const floor = floors.find(candidate => candidate.levelId === room.levelId && candidate.roomId === room.id);
    const usableRadius = room.boundary.radius - boundaryCurve.section.thickness / 2;
    if (usableRadius <= 0) fail(`rooms.${room.id}.boundary.radius`, 'must exceed half the ring wall thickness');
    floor.regions[0].radius = usableRadius;
  }
  for (const throat of radialThroats) {
    const floor = floors.find(candidate => candidate.levelId === throat.levelId &&
      candidate.roomId === throat.curveRoomId);
    floor.extensions.push({ kind: 'polygon-rect', id: throat.floorPatch.id,
      bounds: { ...throat.floorPatch.bounds } });
    floor.walkableVolumeIds.push(`occupied:${throat.id}`);
  }
  const beamMembers = buildBeamMembers(plan, levelsById);
  for (const floor of floors) {
    for (const memberId of floor.bearing.intermediateSupportIds) {
      const member = beamMembers.find(candidate => candidate.id === memberId);
      if (!member) fail(`rooms.${floor.roomId}.floorStructure`, `unknown beam member ${memberId}`);
      if (member.levelId !== floor.levelId || member.role === 'joist')
        fail(`rooms.${floor.roomId}.floorStructure`,
          `intermediate support ${memberId} must be a same-level non-joist member`);
    }
    for (const memberId of floor.joists.memberIds) {
      const member = beamMembers.find(candidate => candidate.id === memberId);
      if (!member) fail(`rooms.${floor.roomId}.floorStructure`, `unknown beam member ${memberId}`);
      if (member.levelId !== floor.levelId || member.role !== 'joist')
        fail(`rooms.${floor.roomId}.floorStructure`,
          `joist member ${memberId} must be a same-level role:joist member`);
    }
  }
  const stairs = buildStairs(plan, levelsById, roomsById, beamMembers);
  const fixtures = flatRecordList(plan.fixtures, levelsById, 'fixtures');
  const roofs = flatRecordList(plan.roofs, levelsById, 'roofs');
  const localLights = flatRecordList(plan.localLights, levelsById, 'localLights');
  const junctionMap = new Map(junctions.map(junction =>
    [`${junction.levelId}:${vertexKey(...junction.position)}`, junction]));
  const wallModules = mergeWallModules(walls, junctionMap);
  const portals = buildPortals(levelsById, roomsById, walls, wallModules, radialThroats);
  const openBoundaries = buildOpenBoundaries(walls);
  for (const boundary of openBoundaries)
    for (const roomId of boundary.roomIds) {
      const floor = floors.find(candidate => candidate.levelId === boundary.levelId && candidate.roomId === roomId);
      if (floor) floor.openBoundaryIds.push(boundary.id);
    }
  const verticalVoids = buildVerticalVoids(plan, levelsById, roomsById, stairs);
  for (const voidRecord of verticalVoids)
    for (const floorId of voidRecord.penetratedFloorIds) {
      const floor = floors.find(candidate => candidate.id === floorId);
      if (!floor) fail(`verticalVoids.${voidRecord.id}`, `unknown penetrated floor ${floorId}`);
      for (let index = 0; index < voidRecord.footprints.length; ++index) {
        const footprint = { ...voidRecord.footprints[index] };
        const stair = voidRecord.kind === 'stair'
          ? stairs.find(candidate => `vertical-void:${candidate.sourceId}` === voidRecord.id) : null;
        const replacementLanding = stair?.landings.find(landing => landing.kind !== 'lower' &&
          landing.bounds.x === footprint.x && landing.bounds.z === footprint.z &&
          landing.bounds.width === footprint.width && landing.bounds.depth === footprint.depth);
        floor.holes.push({
          id: `floor-hole:${idToken(voidRecord.id)}:${index}`,
          voidId: voidRecord.id, kind: voidRecord.kind,
          footprint, regions: [{ ...footprint }],
          replacementLandingId: replacementLanding?.id ?? null,
        });
      }
    }
  const roomGraph = buildRoomGraph(plan, allRooms, portals, stairs, beamMembers, fixtures, floors);
  const manifestLevels = levels.map(level => ({ id: level.id, baseY: level.baseY, height: level.height }));
  const rooms = allRooms.map(room => ({
    id: room.id, levelId: room.levelId, use: room.use, required: room.required !== false,
    walkable: !room.openToBelow, openToBelow: room.openToBelow,
    lowerRoomId: room.openToBelow ? room.lowerRoomId : null,
    floorType: room.openToBelow ? null : (room.floorType ?? 'stone'), boundary: roomBoundary(room),
  })).sort((a, b) => cmp(a.id, b.id));
  const manifest = {
    schema: CASTLE_MANIFEST_SCHEMA, planId, seed, style,
    levels: manifestLevels, rooms, walls, wallModules, junctions, floors,
    curves, radialThroats, curveTransitions, portals, openBoundaries,
    beamMembers, stairs, verticalVoids, roofs, fixtures, localLights,
    occupiedVolumes: collectOccupiedVolumes(levels, stairs, fixtures,
      radialThroats, portals, floors),
    roomGraph, walkRoute: roomGraph.walkRoute,
  };
  return manifest;
}

export function validatePlan(plan) {
  compile(plan);
  return { valid: true, errors: [] };
}

export function compilePlan(plan) {
  return compile(plan);
}

export function planToJSON(manifest, space = 2) {
  if (manifest?.schema !== CASTLE_MANIFEST_SCHEMA) fail('manifest.schema', `must be ${CASTLE_MANIFEST_SCHEMA}`);
  return `${JSON.stringify(manifest, null, space)}\n`;
}

function svgEscape(value) {
  return String(value).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/"/g, '&quot;');
}

export function planToSVG(manifest, options = {}) {
  if (manifest?.schema !== CASTLE_MANIFEST_SCHEMA) fail('manifest.schema', `must be ${CASTLE_MANIFEST_SCHEMA}`);
  const levelId = options.levelId ?? manifest.levels[0]?.id;
  const scale = options.scale ?? 32, pad = options.padding ?? 24;
  const walls = manifest.walls.filter(wall => wall.levelId === levelId);
  const curves = manifest.curves.filter(curve => curve.levelId === levelId);
  const rooms = manifest.rooms.filter(room => room.levelId === levelId);
  const points = walls.flatMap(wall => [wall.from, wall.to]);
  for (const curve of curves) {
    points.push([curve.center[0] - curve.radius, curve.center[1] - curve.radius]);
    points.push([curve.center[0] + curve.radius, curve.center[1] + curve.radius]);
  }
  if (points.length === 0) fail('svg', `level ${levelId} has no drawable boundaries`);
  const minX = Math.min(...points.map(p => p[0])), maxX = Math.max(...points.map(p => p[0]));
  const minZ = Math.min(...points.map(p => p[1])), maxZ = Math.max(...points.map(p => p[1]));
  const width = (maxX - minX) * scale + pad * 2, height = (maxZ - minZ) * scale + pad * 2;
  const sx = x => pad + (x - minX) * scale;
  const sz = z => height - pad - (z - minZ) * scale;
  const lines = walls.map(wall => {
    const klass = wall.kind === 'wall' ? wall.boundary : wall.kind;
    const override = wall.overrideId ? ` data-override="${svgEscape(wall.overrideId)}"` : '';
    return `  <line id="${svgEscape(wall.id)}" class="${klass}"${override} x1="${sx(wall.from[0])}" y1="${sz(wall.from[1])}" x2="${sx(wall.to[0])}" y2="${sz(wall.to[1])}"/>`;
  });
  const arcs = curves.map(curve => {
    if (curve.kind === 'ring') return `  <circle id="${svgEscape(curve.id)}" class="curve" cx="${sx(curve.center[0])}" cy="${sz(curve.center[1])}" r="${curve.radius * scale}"/>`;
    const [a, b] = curve.endpoints;
    return `  <path id="${svgEscape(curve.id)}" class="curve" d="M ${sx(a.position[0])} ${sz(a.position[1])} A ${curve.radius * scale} ${curve.radius * scale} 0 0 ${curve.clockwise ? 1 : 0} ${sx(b.position[0])} ${sz(b.position[1])}"/>`;
  });
  const labels = rooms.map(room => {
    let x, z;
    if (room.boundary.kind === 'circle') [x, z] = room.boundary.center;
    else {
      x = room.boundary.cells.reduce((sum, cell) => sum + cell[0] + 0.5, 0) / room.boundary.cells.length;
      z = room.boundary.cells.reduce((sum, cell) => sum + cell[1] + 0.5, 0) / room.boundary.cells.length;
    }
    return `  <text x="${sx(x)}" y="${sz(z)}">${svgEscape(room.id)}</text>`;
  });
  return [
    '<?xml version="1.0" encoding="UTF-8"?>',
    `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${width} ${height}" role="img" aria-label="${svgEscape(manifest.planId)} ${svgEscape(levelId)} plan">`,
    '  <style>.exterior{stroke:#24211d;stroke-width:7}.partition{stroke:#62594d;stroke-width:5}.door,.arch,.open{stroke:#39a96b;stroke-width:5}.window{stroke:#448bd1;stroke-width:5}.curve{fill:none;stroke:#9a6235;stroke-width:7}text{font:12px sans-serif;text-anchor:middle;fill:#181512}line{stroke-linecap:square}</style>',
    `  <rect width="${width}" height="${height}" fill="#f4efe4"/>`,
    ...lines, ...arcs, ...labels, '</svg>', '',
  ].join('\n');
}

const EMIT_COLLECTIONS = Object.freeze([
  ['walls', 'wall'], ['wallModules', 'wallModule'], ['junctions', 'junction'],
  ['floors', 'floor'], ['curves', 'curve'], ['radialThroats', 'radialThroat'],
  ['curveTransitions', 'curveTransition'], ['portals', 'portal'],
  ['openBoundaries', 'openBoundary'], ['beamMembers', 'beamMember'],
  ['stairs', 'stair'], ['verticalVoids', 'verticalVoid'],
  ['roofs', 'roof'], ['fixtures', 'fixture'],
  ['localLights', 'localLight'],
]);

export function emitManifest(manifest, emitters) {
  if (manifest?.schema !== CASTLE_MANIFEST_SCHEMA) fail('manifest.schema', `must be ${CASTLE_MANIFEST_SCHEMA}`);
  if (!emitters || typeof emitters !== 'object') fail('emitters', 'must be an object');
  const emitted = [];
  for (const [collection, name] of EMIT_COLLECTIONS) {
    const emitter = emitters[name];
    if (emitter === undefined) continue;
    if (typeof emitter !== 'function') fail(`emitters.${name}`, 'must be a function');
    for (const record of manifest[collection]) emitted.push(emitter(record, manifest));
  }
  return emitted;
}

export function manifestPartRecipes(manifest, moduleNames = {}) {
  const recipes = [];
  emitManifest(manifest, Object.fromEntries(EMIT_COLLECTIONS.map(([collection, name]) => [name, record => {
    const module = moduleNames[name];
    if (!module) return null;
    const params = {
      manifestId: manifest.planId,
      recordId: record.id,
      recordKind: name,
      recordIndex: manifest[collection].findIndex(candidate => candidate.id === record.id),
      seed: manifest.seed,
    };
    const recipe = { module, params };
    recipes.push(recipe);
    return recipe;
  }])));
  return recipes;
}
