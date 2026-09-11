// Deterministic composition of complete castle-plan manifests in independent
// rigid frames. Detailed connector meshes/colliders deliberately belong to
// castle_connector_kit.js; this module publishes their validated geometry.
import { compilePlan } from './castle_plan.js';
import {
  makePlanarFrame,
  planarFrameMatrix,
  rotateXZ,
  solvePlanarFrame,
  transformAabbToObb,
  transformPoint,
  transformPointXZ,
  transformSpotLight,
  transformVector,
} from './castle_frames.js';

export const CASTLE_SITE_SCHEMA = 'matter.castle-site/v1';
export const CASTLE_SITE_MANIFEST_SCHEMA = 'matter.castle-site-manifest/v1';
export const CASTLE_SITE_CAPSULE_RADIUS = 0.4;
export const CASTLE_SITE_CLEARANCE = 0.2;
export const CASTLE_SITE_MIN_CLEAR_WIDTH =
  2 * (CASTLE_SITE_CAPSULE_RADIUS + CASTLE_SITE_CLEARANCE);

const EPSILON = 1e-8;

function fail(path, message) {
  throw new Error(`castle site ${path}: ${message}`);
}

function finite(value, path) {
  if (typeof value !== 'number' || !Number.isFinite(value)) fail(path, 'must be finite');
  return value;
}

function positive(value, path) {
  finite(value, path);
  if (value <= 0) fail(path, 'must be positive');
  return value;
}

function string(value, path) {
  if (typeof value !== 'string' || value.length === 0) fail(path, 'must be a non-empty string');
  return value;
}

function point2(value, path) {
  if (!Array.isArray(value) || value.length !== 2) fail(path, 'must be [x,z]');
  return value.map((component, index) => finite(component, `${path}[${index}]`));
}

function cmp(a, b) { return a < b ? -1 : a > b ? 1 : 0; }
function stableSort(values, key) { return [...values].sort((a, b) => cmp(key(a), key(b))); }
function idToken(value) { return encodeURIComponent(String(value)).replace(/%/g, '~'); }
function near(a, b) { return Math.abs(a - b) <= EPSILON; }
function samePoint(a, b) {
  return a.length === b.length && a.every((component, index) => near(component, b[index]));
}
function add2(a, b) { return [a[0] + b[0], a[1] + b[1]]; }
function subtract2(a, b) { return [a[0] - b[0], a[1] - b[1]]; }
function scale2(a, scalar) { return [a[0] * scalar, a[1] * scalar]; }
function dot2(a, b) { return a[0] * b[0] + a[1] * b[1]; }
function length2(a) { return Math.hypot(a[0], a[1]); }
function normalize2(a, path) {
  const length = length2(a);
  if (!(length > EPSILON)) fail(path, 'must have non-zero length');
  return [a[0] / length, a[1] / length];
}
function midpoint2(a, b) { return [(a[0] + b[0]) / 2, (a[1] + b[1]) / 2]; }
function point3FromXZ(point, y) { return [point[0], y, point[1]]; }

function polygonArea(polygon) {
  let twice = 0;
  for (let i = 0; i < polygon.length; ++i) {
    const a = polygon[i], b = polygon[(i + 1) % polygon.length];
    twice += a[0] * b[1] - a[1] * b[0];
  }
  return twice / 2;
}

function cross(origin, a, b) {
  return (a[0] - origin[0]) * (b[1] - origin[1]) -
    (a[1] - origin[1]) * (b[0] - origin[0]);
}

function convexHull(points, path) {
  const unique = stableSort(points.map((point, index) => point2(point, `${path}[${index}]`)),
    point => `${point[0].toPrecision(15)},${point[1].toPrecision(15)}`)
    .filter((point, index, all) => index === 0 || !samePoint(point, all[index - 1]));
  if (unique.length < 3) fail(path, 'must contain at least three distinct points');
  const sorted = [...unique].sort((a, b) => a[0] - b[0] || a[1] - b[1]);
  const half = pointsIn => {
    const result = [];
    for (const point of pointsIn) {
      while (result.length >= 2 && cross(result[result.length - 2], result[result.length - 1], point) <= EPSILON)
        result.pop();
      result.push(point);
    }
    return result;
  };
  const hull = [...half(sorted).slice(0, -1), ...half([...sorted].reverse()).slice(0, -1)];
  if (hull.length < 3 || polygonArea(hull) <= EPSILON) fail(path, 'must have positive area');
  return hull;
}

function isConvexPolygon(polygon) {
  if (!Array.isArray(polygon) || polygon.length < 3) return false;
  let sign = 0;
  for (let i = 0; i < polygon.length; ++i) {
    const turn = cross(polygon[i], polygon[(i + 1) % polygon.length],
      polygon[(i + 2) % polygon.length]);
    if (Math.abs(turn) <= EPSILON) continue;
    const next = Math.sign(turn);
    if (sign && next !== sign) return false;
    sign = next;
  }
  return sign !== 0;
}

function polygonMinimumWidth(polygon) {
  let minimum = Infinity;
  for (let i = 0; i < polygon.length; ++i) {
    const a = polygon[i], b = polygon[(i + 1) % polygon.length];
    const edge = subtract2(b, a), edgeLength = length2(edge);
    if (edgeLength <= EPSILON) return 0;
    let maximumDistance = 0;
    for (const point of polygon)
      maximumDistance = Math.max(maximumDistance, Math.abs(cross(a, b, point)) / edgeLength);
    minimum = Math.min(minimum, maximumDistance);
  }
  return minimum;
}

function projectionsOverlap(a, b, axis, positiveOnly) {
  const pa = a.map(point => dot2(point, axis));
  const pb = b.map(point => dot2(point, axis));
  const overlap = Math.min(Math.max(...pa), Math.max(...pb)) -
    Math.max(Math.min(...pa), Math.min(...pb));
  return positiveOnly ? overlap > EPSILON : overlap >= -EPSILON;
}

function convexPolygonsOverlap(a, b, positiveOnly = true) {
  for (const polygon of [a, b]) for (let i = 0; i < polygon.length; ++i) {
    const edge = subtract2(polygon[(i + 1) % polygon.length], polygon[i]);
    const axis = normalize2([-edge[1], edge[0]], 'polygon edge');
    if (!projectionsOverlap(a, b, axis, positiveOnly)) return false;
  }
  return true;
}

function transformPolygon(frame, polygon) {
  return polygon.map(point => transformPointXZ(frame, point));
}

function levelFor(manifest, levelId, path) {
  const level = manifest.levels.find(candidate => candidate.id === levelId);
  if (!level) fail(path, `unknown level ${levelId}`);
  return level;
}

function socketKey(reference) {
  return `${reference.wing}:${reference.level}:${reference.portal}`;
}

function normalizeSocketReference(value, path, wingRequired = true) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) fail(path, 'must be an object');
  const result = {
    level: string(value.level, `${path}.level`),
    portal: string(value.portal, `${path}.portal`),
  };
  if (wingRequired) result.wing = string(value.wing, `${path}.wing`);
  else if (value.wing !== undefined) result.wing = string(value.wing, `${path}.wing`);
  return result;
}

function outsideThreshold(portal, roomThresholds, path) {
  const roomPoints = Object.values(roomThresholds);
  const point = portal.thresholds.find(candidate =>
    !roomPoints.some(roomPoint => samePoint(candidate, roomPoint)));
  if (!point) fail(path, 'cannot derive exterior threshold from portal geometry');
  return [...point];
}

function authoredPortalTangent(plan, reference, outward) {
  const level = plan.levels.find(candidate => candidate.id === reference.level);
  const edge = level?.edgeOverrides?.find(candidate => candidate.id === reference.portal);
  if (edge?.from && edge?.to) {
    const direction = [edge.to[0] - edge.from[0], edge.to[1] - edge.from[1]];
    if (length2(direction) > EPSILON) return normalize2(direction, 'portal tangent');
  }
  return normalize2([-outward[1], outward[0]], 'portal tangent');
}

function localSocket(wing, reference, path) {
  const matches = wing.manifest.portals.filter(candidate =>
    candidate.levelId === reference.level && candidate.sourceId === reference.portal);
  if (matches.length !== 1)
    fail(path, matches.length ? `ambiguous portal ${reference.level}:${reference.portal}` :
      `missing portal ${reference.level}:${reference.portal}`);
  const portal = matches[0];
  if (!portal.rooms.includes('outside')) fail(path, 'portal is not exterior');
  if (portal.kind !== 'door' && portal.kind !== 'arch')
    fail(path, `portal kind ${portal.kind} cannot become a compound link`);
  if (portal.clearWidth + EPSILON < CASTLE_SITE_MIN_CLEAR_WIDTH)
    fail(path, `clear width ${portal.clearWidth}m is narrower than ${CASTLE_SITE_MIN_CLEAR_WIDTH}m`);
  const realRooms = portal.rooms.filter(room => room !== 'outside');
  if (realRooms.length !== 1) fail(path, 'exterior portal must connect exactly one room');
  const roomId = realRooms[0], inside = portal.roomThresholds[roomId];
  if (!inside) fail(path, `portal has no interior threshold for ${roomId}`);
  const outside = outsideThreshold(portal, portal.roomThresholds, path);
  const outward3 = transformVector({ yawDeg: 0 },
    [outside[0] - inside[0], 0, outside[2] - inside[2]]);
  const outward = normalize2([outward3[0], outward3[2]], `${path}.outward`);
  const tangent = authoredPortalTangent(wing.plan, reference, outward);
  const center = [(inside[0] + outside[0]) / 2, inside[1], (inside[2] + outside[2]) / 2];
  const half = portal.clearWidth / 2;
  return {
    wing: wing.id, level: reference.level, portalId: reference.portal, roomId,
    inside: [...inside], outside, center,
    segment: [
      [center[0] - tangent[0] * half, center[2] - tangent[1] * half],
      [center[0] + tangent[0] * half, center[2] + tangent[1] * half],
    ],
    tangent, outward, clearWidth: portal.clearWidth, clearHeight: portal.clearHeight,
    wallThickness: Math.hypot(outside[0] - inside[0], outside[2] - inside[2]),
    hostModules: [...portal.hostModuleIds],
    jambOwner: `wing:${wing.id}:portal:${idToken(reference.level)}:${idToken(reference.portal)}`,
    portal,
  };
}

function worldSocket(wing, reference, path) {
  const socket = localSocket(wing, reference, path);
  const tangent = rotateXZ(socket.tangent, wing.frame.yawDeg);
  const outward = rotateXZ(socket.outward, wing.frame.yawDeg);
  return {
    ...socket,
    inside: transformPoint(wing.frame, socket.inside),
    outside: transformPoint(wing.frame, socket.outside),
    center: transformPoint(wing.frame, socket.center),
    segment: socket.segment.map(point => transformPointXZ(wing.frame, point)),
    tangent, outward,
    hostModules: socket.hostModules.map(id => `${wing.id}:${id}`),
    jambOwner: `wing:${wing.id}:portal:${idToken(reference.level)}:${idToken(reference.portal)}`,
  };
}

function normalizeWings(site) {
  if (!Array.isArray(site.wings) || site.wings.length === 0) fail('wings', 'must be non-empty');
  const ids = new Set();
  return stableSort(site.wings.map((source, index) => {
    const path = `wings[${index}]`;
    if (!source || typeof source !== 'object' || Array.isArray(source)) fail(path, 'must be an object');
    const id = string(source.id, `${path}.id`);
    if (ids.has(id)) fail(`${path}.id`, `duplicate id ${id}`);
    ids.add(id);
    if (!source.plan || typeof source.plan !== 'object') fail(`${path}.plan`, 'must be a castle plan');
    if ((source.frame === undefined) === (source.placement === undefined))
      fail(path, 'must declare exactly one of frame or placement');
    return { id, plan: source.plan, manifest: compilePlan(source.plan), source, frame: null };
  }), wing => wing.id);
}

function solveWingFrames(wings, angleStep) {
  const byId = new Map(wings.map(wing => [wing.id, wing]));
  const visiting = new Set(), solved = new Set();
  const solve = wing => {
    if (solved.has(wing.id)) return;
    if (visiting.has(wing.id)) fail(`wings.${wing.id}.placement`, 'cyclic placement dependency');
    visiting.add(wing.id);
    if (wing.source.frame !== undefined) {
      wing.frame = makePlanarFrame(wing.source.frame);
      if (!near(wing.frame.yawDeg / angleStep, Math.round(wing.frame.yawDeg / angleStep)))
        fail(`wings.${wing.id}.frame.yawDeg`, `must snap to angleStep ${angleStep}`);
    } else {
      const placement = wing.source.placement;
      if (!placement || typeof placement !== 'object' || Array.isArray(placement))
        fail(`wings.${wing.id}.placement`, 'must be an object');
      const movingRef = normalizeSocketReference(placement.socket,
        `wings.${wing.id}.placement.socket`, false);
      if (movingRef.wing !== undefined && movingRef.wing !== wing.id)
        fail(`wings.${wing.id}.placement.socket.wing`, 'must match the moving wing');
      movingRef.wing = wing.id;
      const targetRef = normalizeSocketReference(placement.relativeTo,
        `wings.${wing.id}.placement.relativeTo`);
      const targetWing = byId.get(targetRef.wing);
      if (!targetWing) fail(`wings.${wing.id}.placement.relativeTo.wing`, `unknown wing ${targetRef.wing}`);
      solve(targetWing);
      const yawDeg = finite(placement.yawDeg, `wings.${wing.id}.placement.yawDeg`);
      if (!near(yawDeg / angleStep, Math.round(yawDeg / angleStep)))
        fail(`wings.${wing.id}.placement.yawDeg`, `must snap to angleStep ${angleStep}`);
      const target = worldSocket(targetWing, targetRef, `wings.${wing.id}.placement.relativeTo`);
      const moving = localSocket(wing, movingRef, `wings.${wing.id}.placement.socket`);
      const outset = finite(placement.outset ?? 0, `wings.${wing.id}.placement.outset`);
      const lateral = finite(placement.lateral ?? 0, `wings.${wing.id}.placement.lateral`);
      const desired = [
        target.center[0] + target.outward[0] * outset + target.tangent[0] * lateral,
        target.center[1],
        target.center[2] + target.outward[1] * outset + target.tangent[1] * lateral,
      ];
      wing.frame = solvePlanarFrame(moving.center, desired, yawDeg);
    }
    visiting.delete(wing.id);
    solved.add(wing.id);
  };
  for (const wing of wings) solve(wing);
}

function wingRoomVolumes(wing) {
  const result = [];
  for (const volume of wing.manifest.occupiedVolumes) {
    if (volume.kind === 'room-cell') {
      const b = volume.bounds;
      result.push({
        id: `${wing.id}:${volume.id}`, wing: wing.id, levelId: volume.levelId,
        minY: b.minY + wing.frame.origin[1], maxY: b.maxY + wing.frame.origin[1],
        polygon: transformPolygon(wing.frame,
          [[b.minX, b.minZ], [b.maxX, b.minZ], [b.maxX, b.maxZ], [b.minX, b.maxZ]]),
      });
    } else if (volume.kind === 'room-circle') {
      const local = Array.from({ length: 24 }, (_, index) => {
        const angle = index * Math.PI * 2 / 24;
        return [volume.center[0] + Math.cos(angle) * volume.radius,
          volume.center[1] + Math.sin(angle) * volume.radius];
      });
      result.push({ id: `${wing.id}:${volume.id}`, wing: wing.id, levelId: volume.levelId,
        minY: volume.minY + wing.frame.origin[1], maxY: volume.maxY + wing.frame.origin[1],
        polygon: transformPolygon(wing.frame, local) });
    }
  }
  return result;
}

function yRangesOverlap(a, b) {
  return a.minY < b.maxY - EPSILON && b.minY < a.maxY - EPSILON;
}

function validateWingOverlap(wings) {
  const volumes = wings.flatMap(wingRoomVolumes);
  for (let i = 0; i < volumes.length; ++i) for (let j = i + 1; j < volumes.length; ++j) {
    const a = volumes[i], b = volumes[j];
    if (a.wing === b.wing || !yRangesOverlap(a, b)) continue;
    if (convexPolygonsOverlap(a.polygon, b.polygon, true))
      fail('wings', `positive-area overlap between ${a.id} and ${b.id}`);
  }
  return volumes;
}

function insideFaceSegment(mouth) {
  const center = [mouth.center[0], mouth.center[2]];
  const inside = [mouth.inside[0], mouth.inside[2]];
  const offset = subtract2(inside, center);
  return mouth.segment.map(point => add2(point, offset));
}

function segmentMatches(edge, segment) {
  return (samePoint(edge[0], segment[0]) && samePoint(edge[1], segment[1])) ||
    (samePoint(edge[0], segment[1]) && samePoint(edge[1], segment[0]));
}

function buildWallSpans(id, polygon, mouthSegments, thickness, baseY, height, material) {
  const spans = [];
  for (let index = 0; index < polygon.length; ++index) {
    const segment = [polygon[index], polygon[(index + 1) % polygon.length]];
    if (mouthSegments.some(mouth => segmentMatches(segment, mouth))) continue;
    const tangent = normalize2(subtract2(segment[1], segment[0]), `connections.${id}.wallSpans`);
    const normal = [-tangent[1], tangent[0]];
    const spanId = `connector:${id}:wall:${spans.length}`;
    spans.push({
      id: spanId, segment: segment.map(point => [...point]), tangent, normal,
      thickness, height, material,
      courseOrigin: [segment[0][0], baseY, segment[0][1]],
      cornerOwners: [`${spanId}:start`, `${spanId}:end`],
      jambOwners: [`connector:${id}:mouth:a:jambs`, `connector:${id}:mouth:b:jambs`],
      trimPlanes: [
        { normal: [-tangent[0], -tangent[1]], offset: -dot2(segment[0], tangent), keepSign: 1 },
        { normal: [tangent[0], tangent[1]], offset: dot2(segment[1], tangent), keepSign: 1 },
      ],
    });
  }
  if (spans.length < 2) fail(`connections.${id}.wallSpans`, 'needs two finite side spans');
  return spans;
}

function publicMouth(mouth, connectorId, side) {
  const { portal, center, clearWidth, roomId, ...record } = mouth;
  return { ...record, jambOwner: `connector:${connectorId}:mouth:${side}:jambs` };
}

function buildConnector(connection, index, byId) {
  const path = `connections[${index}]`;
  if (!connection || typeof connection !== 'object' || Array.isArray(connection)) fail(path, 'must be an object');
  const id = string(connection.id, `${path}.id`);
  const aRef = normalizeSocketReference(connection.a, `${path}.a`);
  const bRef = normalizeSocketReference(connection.b, `${path}.b`);
  const aWing = byId.get(aRef.wing), bWing = byId.get(bRef.wing);
  if (!aWing) fail(`${path}.a.wing`, `unknown wing ${aRef.wing}`);
  if (!bWing) fail(`${path}.b.wing`, `unknown wing ${bRef.wing}`);
  if (aWing === bWing) fail(path, 'must connect two different wings');
  const a = worldSocket(aWing, aRef, `${path}.a`), b = worldSocket(bWing, bRef, `${path}.b`);
  const aLevel = levelFor(aWing.manifest, aRef.level, `${path}.a.level`);
  const bLevel = levelFor(bWing.manifest, bRef.level, `${path}.b.level`);
  const aBaseY = aLevel.baseY + aWing.frame.origin[1];
  const bBaseY = bLevel.baseY + bWing.frame.origin[1];
  if (!near(aBaseY, bBaseY)) fail(path, `incompatible elevations ${aBaseY} and ${bBaseY}`);
  const centerDelta = [b.center[0] - a.center[0], b.center[2] - a.center[2]];
  if (length2(centerDelta) <= EPSILON) fail(path, 'portal mouths coincide');
  if (dot2(centerDelta, a.outward) <= EPSILON || dot2(scale2(centerDelta, -1), b.outward) <= EPSILON)
    fail(path, 'portal mouths do not face the connector without intruding through a wing');
  const aFace = insideFaceSegment(a), bFace = insideFaceSegment(b);
  const clearPolygon = convexHull([...aFace, ...bFace], `${path}.clearPolygon`);
  if (!isConvexPolygon(clearPolygon)) fail(`${path}.clearPolygon`, 'must be convex');
  const minimumWidth = polygonMinimumWidth(clearPolygon);
  if (minimumWidth + EPSILON < CASTLE_SITE_MIN_CLEAR_WIDTH)
    fail(`${path}.clearPolygon`, `narrow join ${minimumWidth}m cannot fit radius 0.4 capsule with 0.2m clearance`);
  const clearHeight = positive(connection.height ?? Math.min(a.clearHeight, b.clearHeight), `${path}.height`);
  if (clearHeight > Math.min(a.clearHeight, b.clearHeight) + EPSILON)
    fail(`${path}.height`, 'exceeds a portal clear height');
  const wallThickness = positive(connection.wallThickness ?? Math.max(a.wallThickness, b.wallThickness),
    `${path}.wallThickness`);
  const wallMaterial = connection.wallMaterial ?? 'castle.stone';
  const wallSpans = buildWallSpans(id, clearPolygon, [aFace, bFace], wallThickness,
    aBaseY, clearHeight, wallMaterial);
  const floorMaterial = connection.floor ?? 'stone';
  const floorThickness = positive(connection.floorThickness ?? 0.25, `${path}.floorThickness`);
  const roof = connection.roof && typeof connection.roof === 'object'
    ? { ...connection.roof, material: connection.roof.material ?? 'slate', overhang: connection.roof.overhang ?? 0.2 }
    : { kind: 'low-hip', rise: 0.8, material: 'slate', overhang: 0.2 };
  positive(roof.rise, `${path}.roof.rise`);
  const routeWaypoints = [a.inside, a.outside, b.outside, b.inside].map(point => [...point]);
  return {
    id, level: aRef.level === bRef.level ? aRef.level : `${aRef.level}|${bRef.level}`,
    baseY: aBaseY, clearPolygon, minimumWidth,
    mouths: [publicMouth(a, id, 'a'), publicMouth(b, id, 'b')], wallSpans,
    floor: { thickness: floorThickness, material: floorMaterial, owner: `connector:${id}:floor` },
    clearHeight, roof, routeWaypoints,
    _rooms: [`${a.wing}:${a.roomId}`, `${b.wing}:${b.roomId}`],
  };
}

function validateConnectorIntrusion(connectors, wingVolumes) {
  for (const connector of connectors) {
    const participantWings = new Set(connector.mouths.map(mouth => mouth.wing));
    const volume = { minY: connector.baseY, maxY: connector.baseY + connector.clearHeight };
    for (const wingVolume of wingVolumes) {
      if (participantWings.has(wingVolume.wing) || !yRangesOverlap(volume, wingVolume)) continue;
      if (convexPolygonsOverlap(connector.clearPolygon, wingVolume.polygon, true))
        fail(`connections.${connector.id}`, `intrudes into wing volume ${wingVolume.id}`);
    }
  }
}

function namespaceRoom(wingId, roomId) { return roomId === 'outside' ? 'outside' : `${wingId}:${roomId}`; }

function transformGraphEdge(wing, edge) {
  const roomThresholds = Object.fromEntries(Object.entries(edge.roomThresholds || {})
    .map(([roomId, point]) => [namespaceRoom(wing.id, roomId), transformPoint(wing.frame, point)]));
  const rooms = edge.rooms.map(roomId => namespaceRoom(wing.id, roomId)).sort();
  let routeWaypoints;
  if (edge.kind === 'stair') {
    const stair = wing.manifest.stairs.find(candidate => candidate.id === edge.portalVolumeId);
    routeWaypoints = stair?.route?.waypoints?.map(point => transformPoint(wing.frame, point));
  }
  if (!routeWaypoints) routeWaypoints = edge.thresholds.map(point => transformPoint(wing.frame, point));
  return {
    ...edge, id: `${wing.id}:${edge.id}`, sourceId: `${wing.id}:${edge.sourceId}`,
    rooms, thresholds: edge.thresholds.map(point => transformPoint(wing.frame, point)),
    roomThresholds, routeWaypoints,
    portalVolumeId: `${wing.id}:${edge.portalVolumeId}`,
    floorIds: (edge.floorIds || []).map(id => `${wing.id}:${id}`),
    sweptVolumeIds: (edge.sweptVolumeIds || []).map(id => `${wing.id}:${id}`),
  };
}

function connectorGraphEdge(connector) {
  const [a, b] = connector.mouths;
  return {
    id: `route:connector:${idToken(connector.id)}`, kind: 'connector', sourceId: connector.id,
    rooms: [...connector._rooms], portalVolumeId: `connector:${connector.id}`,
    floorIds: [`connector:${connector.id}:floor`], clearWidth: connector.minimumWidth,
    thresholds: [a.inside, b.inside].map(point => [...point]),
    roomThresholds: { [connector._rooms[0]]: [...a.inside], [connector._rooms[1]]: [...b.inside] },
    sweptVolumeIds: [`connector:${connector.id}:clearance`],
    routeWaypoints: connector.routeWaypoints.map(point => [...point]),
  };
}

function orientEdgeWaypoints(edge, fromRoom, toRoom) {
  const startsAtFrom = samePoint(edge.routeWaypoints[0], edge.roomThresholds[fromRoom]);
  const endsAtTo = samePoint(edge.routeWaypoints[edge.routeWaypoints.length - 1], edge.roomThresholds[toRoom]);
  if (startsAtFrom && endsAtTo) return edge.routeWaypoints.map(point => [...point]);
  return [...edge.routeWaypoints].reverse().map(point => [...point]);
}

function buildGlobalGraph(wings, connectors, entryRef) {
  const entryWing = wings.find(wing => wing.id === entryRef.wing);
  const entrySocket = worldSocket(entryWing, entryRef, 'entry');
  const entryRoomId = `${entryWing.id}:${entrySocket.roomId}`;
  const nodes = wings.flatMap(wing => wing.manifest.roomGraph.nodes.map(node => ({
    ...node, id: `${wing.id}:${node.id}`, wingId: wing.id,
    levelId: `${wing.id}:${node.levelId}`,
  }))).sort((a, b) => cmp(a.id, b.id));
  const edges = [];
  for (const wing of wings) for (const edge of wing.manifest.roomGraph.edges) {
    if (edge.rooms.includes('outside')) {
      if (wing.id !== entryRef.wing || edge.sourceId !== entryRef.portal) continue;
    }
    edges.push(transformGraphEdge(wing, edge));
  }
  edges.push(...connectors.map(connectorGraphEdge));
  edges.sort((a, b) => cmp(a.id, b.id));
  const adjacency = new Map(nodes.map(node => [node.id, []]));
  for (const edge of edges) {
    const realRooms = edge.rooms.filter(room => room !== 'outside');
    if (realRooms.length === 2) {
      adjacency.get(realRooms[0])?.push({ room: realRooms[1], edge });
      adjacency.get(realRooms[1])?.push({ room: realRooms[0], edge });
    }
  }
  const parent = new Map(), parentEdge = new Map(), reachable = new Set([entryRoomId]);
  const queue = [entryRoomId];
  while (queue.length) {
    const current = queue.shift();
    for (const item of stableSort(adjacency.get(current) || [], value => `${value.room}:${value.edge.id}`)) {
      if (reachable.has(item.room)) continue;
      reachable.add(item.room); parent.set(item.room, current); parentEdge.set(item.room, item.edge);
      queue.push(item.room);
    }
  }
  const missing = nodes.filter(node => node.required && !reachable.has(node.id)).map(node => node.id);
  if (missing.length) fail('roomGraph', `required rooms are unreachable from ${entryRoomId}: ${missing.join(', ')}`);
  const walkRoutes = stableSort([...reachable], value => value).map(roomId => {
    const roomPath = [], edgePath = [];
    for (let cursor = roomId; cursor !== undefined; cursor = parent.get(cursor)) {
      roomPath.push(cursor);
      if (parentEdge.has(cursor)) edgePath.push(parentEdge.get(cursor));
    }
    roomPath.reverse(); edgePath.reverse();
    const waypoints = [[...entrySocket.inside]];
    let currentRoom = entryRoomId;
    for (const edge of edgePath) {
      const nextRoom = edge.rooms.find(room => room !== 'outside' && room !== currentRoom);
      const from = edge.roomThresholds[currentRoom], to = edge.roomThresholds[nextRoom];
      if (!samePoint(waypoints[waypoints.length - 1], from)) waypoints.push([...from]);
      for (const point of orientEdgeWaypoints(edge, currentRoom, nextRoom).slice(1))
        if (!samePoint(waypoints[waypoints.length - 1], point)) waypoints.push(point);
      currentRoom = nextRoom;
    }
    return { roomId, fromEntry: roomPath, edgeIds: edgePath.map(edge => edge.id), waypoints };
  });
  return {
    entryRoomId, entryPortalId: `${entryWing.id}:${entrySocket.portal.id}`,
    nodes, edges, reachableRoomIds: [...reachable].sort(), walkRoutes,
  };
}

function worldWingSummary(wing) {
  const rootTransform = planarFrameMatrix(wing.frame);
  const occupiedVolumes = wing.manifest.occupiedVolumes.map(volume => {
    if (volume.bounds) return { ...volume, id: `${wing.id}:${volume.id}`,
      orientedBounds: transformAabbToObb(wing.frame, volume.bounds) };
    if (volume.center) return { ...volume, id: `${wing.id}:${volume.id}`,
      center: transformPointXZ(wing.frame, volume.center) };
    return { ...volume, id: `${wing.id}:${volume.id}` };
  });
  const fixtures = wing.manifest.fixtures.map(fixture => ({
    ...fixture, id: `${wing.id}:${fixture.id}`,
    ...(fixture.position ? { position: transformPoint(wing.frame, fixture.position) } : {}),
    ...(fixture.direction ? { direction: transformVector(wing.frame, fixture.direction) } : {}),
    ...(fixture.yaw !== undefined ? { yaw: fixture.yaw + wing.frame.yawDeg } : {}),
    ...(fixture.bounds ? { orientedBounds: transformAabbToObb(wing.frame, fixture.bounds) } : {}),
    ...(fixture.clearance ? { orientedClearance: transformAabbToObb(wing.frame, fixture.clearance) } : {}),
  }));
  const localLights = wing.manifest.localLights.map(light => ({
    ...(light.kind === 'spot' ? transformSpotLight(wing.frame, light) : light),
    id: `${wing.id}:${light.id}`,
    ...(light.kind !== 'spot' && light.position ? { position: transformPoint(wing.frame, light.position) } : {}),
  }));
  return { id: wing.id, frame: { origin: [...wing.frame.origin], yawDeg: wing.frame.yawDeg },
    rootTransform, manifest: wing.manifest, world: { occupiedVolumes, fixtures, localLights } };
}

function compile(site) {
  if (!site || typeof site !== 'object' || Array.isArray(site)) fail('', 'must be an object');
  if (site.schema !== CASTLE_SITE_SCHEMA) fail('schema', `must be ${CASTLE_SITE_SCHEMA}`);
  const siteId = string(site.id, 'id');
  const seed = finite(site.seed ?? 0, 'seed');
  if (!Number.isInteger(seed)) fail('seed', 'must be an integer');
  const grid = positive(site.grid ?? 1, 'grid');
  const angleStep = positive(site.angleStep ?? 15, 'angleStep');
  if (!near(360 / angleStep, Math.round(360 / angleStep))) fail('angleStep', 'must divide 360 degrees');
  const wings = normalizeWings(site);
  solveWingFrames(wings, angleStep);
  const byId = new Map(wings.map(wing => [wing.id, wing]));
  const entryRef = normalizeSocketReference(site.entry, 'entry');
  if (!byId.has(entryRef.wing)) fail('entry.wing', `unknown wing ${entryRef.wing}`);
  worldSocket(byId.get(entryRef.wing), entryRef, 'entry');
  if (!Array.isArray(site.connections)) fail('connections', 'must be an array');
  const connectorIds = new Set(), consumed = new Set([socketKey(entryRef)]);
  const connectors = stableSort(site.connections.map((connection, index) => {
    const id = string(connection?.id, `connections[${index}].id`);
    if (connectorIds.has(id)) fail(`connections[${index}].id`, `duplicate id ${id}`);
    connectorIds.add(id);
    for (const side of ['a', 'b']) {
      const reference = normalizeSocketReference(connection[side], `connections[${index}].${side}`);
      const key = socketKey(reference);
      if (consumed.has(key)) fail(`connections[${index}].${side}`, `socket ${key} is already consumed`);
      consumed.add(key);
    }
    return buildConnector(connection, index, byId);
  }), connector => connector.id);
  const wingVolumes = validateWingOverlap(wings);
  validateConnectorIntrusion(connectors, wingVolumes);
  const roomGraph = buildGlobalGraph(wings, connectors, entryRef);
  const entrySocket = worldSocket(byId.get(entryRef.wing), entryRef, 'entry');
  const spawn = [
    entrySocket.inside[0] - entrySocket.outward[0] * 0.5,
    entrySocket.inside[1],
    entrySocket.inside[2] - entrySocket.outward[1] * 0.5,
  ];
  return {
    schema: CASTLE_SITE_MANIFEST_SCHEMA, siteId, seed, grid, angleStep,
    wings: wings.map(worldWingSummary),
    connectors: connectors.map(({ _rooms, ...connector }) => connector),
    roomGraph, walkRoutes: roomGraph.walkRoutes,
    entry: { wing: entryRef.wing, level: entryRef.level, portalId: entryRef.portal,
      roomId: roomGraph.entryRoomId, inside: entrySocket.inside, outside: entrySocket.outside },
    spawn,
  };
}

export function compileSite(site) { return compile(site); }

export function validateSite(site) {
  compile(site);
  return { valid: true, errors: [] };
}

export function siteToJSON(manifest, space = 2) {
  if (manifest?.schema !== CASTLE_SITE_MANIFEST_SCHEMA)
    fail('manifest.schema', `must be ${CASTLE_SITE_MANIFEST_SCHEMA}`);
  return `${JSON.stringify(manifest, null, space)}\n`;
}

function svgEscape(value) {
  return String(value).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/"/g, '&quot;');
}

export function siteToSVG(manifest, options = {}) {
  if (manifest?.schema !== CASTLE_SITE_MANIFEST_SCHEMA)
    fail('manifest.schema', `must be ${CASTLE_SITE_MANIFEST_SCHEMA}`);
  const scale = positive(options.scale ?? 24, 'svg.scale');
  const padding = positive(options.padding ?? 24, 'svg.padding');
  const walls = manifest.wings.flatMap(wing => wing.manifest.walls.map(wall => ({
    wing, wall, from: transformPointXZ(wing.frame, wall.from), to: transformPointXZ(wing.frame, wall.to),
  })));
  const routePoints = manifest.walkRoutes.flatMap(route => route.waypoints.map(point => [point[0], point[2]]));
  const points = [
    ...walls.flatMap(item => [item.from, item.to]),
    ...manifest.connectors.flatMap(connector => connector.clearPolygon),
    ...routePoints,
  ];
  if (!points.length) fail('svg', 'has no drawable geometry');
  const minX = Math.min(...points.map(point => point[0]));
  const maxX = Math.max(...points.map(point => point[0]));
  const minZ = Math.min(...points.map(point => point[1]));
  const maxZ = Math.max(...points.map(point => point[1]));
  const width = (maxX - minX) * scale + padding * 2;
  const height = (maxZ - minZ) * scale + padding * 2;
  const sx = x => padding + (x - minX) * scale;
  const sz = z => height - padding - (z - minZ) * scale;
  const wallLines = walls.map(({ wing, wall, from, to }) =>
    `  <line class="wall ${svgEscape(wall.kind)}" data-wing="${svgEscape(wing.id)}" x1="${sx(from[0])}" y1="${sz(from[1])}" x2="${sx(to[0])}" y2="${sz(to[1])}"/>`);
  const connectorPolygons = manifest.connectors.map(connector =>
    `  <polygon class="connector" data-link="${svgEscape(connector.id)}" points="${connector.clearPolygon.map(point => `${sx(point[0])},${sz(point[1])}`).join(' ')}"/>`);
  const labels = manifest.wings.flatMap(wing => wing.manifest.rooms.map(room => {
    let local;
    if (room.boundary.kind === 'circle') local = room.boundary.center;
    else local = [
      room.boundary.cells.reduce((sum, cell) => sum + cell[0] + 0.5, 0) / room.boundary.cells.length,
      room.boundary.cells.reduce((sum, cell) => sum + cell[1] + 0.5, 0) / room.boundary.cells.length,
    ];
    const world = transformPointXZ(wing.frame, local);
    return `  <text data-room="${svgEscape(`${wing.id}:${room.id}`)}" x="${sx(world[0])}" y="${sz(world[1])}">${svgEscape(`${wing.id} ${room.use} ${wing.frame.yawDeg}°`)}</text>`;
  }));
  const routes = manifest.walkRoutes.filter(route => route.waypoints.length > 1).map(route =>
    `  <polyline class="route" data-room="${svgEscape(route.roomId)}" points="${route.waypoints.map(point => `${sx(point[0])},${sz(point[2])}`).join(' ')}"/>`);
  return [
    '<?xml version="1.0" encoding="UTF-8"?>',
    `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${width} ${height}" role="img" aria-label="${svgEscape(manifest.siteId)} castle site">`,
    '  <style>.wall{stroke:#29251f;stroke-width:5;fill:none}.door,.arch,.open{stroke:#369b63}.connector{fill:#d8b978;fill-opacity:.75;stroke:#8a5a24;stroke-width:2}.route{fill:none;stroke:#d33856;stroke-width:1.5;stroke-dasharray:5 3}text{font:10px sans-serif;text-anchor:middle;fill:#171411}</style>',
    `  <rect width="${width}" height="${height}" fill="#f4efe4"/>`,
    ...connectorPolygons, ...wallLines, ...routes, ...labels, '</svg>', '',
  ].join('\n');
}

export function emitSite(manifest, emitters = {}) {
  if (manifest?.schema !== CASTLE_SITE_MANIFEST_SCHEMA)
    fail('manifest.schema', `must be ${CASTLE_SITE_MANIFEST_SCHEMA}`);
  const emitted = [];
  if (emitters.wing !== undefined) {
    if (typeof emitters.wing !== 'function') fail('emitters.wing', 'must be a function');
    for (const wing of manifest.wings) emitted.push(emitters.wing(wing, manifest));
  }
  if (emitters.connector !== undefined) {
    if (typeof emitters.connector !== 'function') fail('emitters.connector', 'must be a function');
    for (const connector of manifest.connectors) emitted.push(emitters.connector(connector, manifest));
  }
  return emitted;
}

export function sitePartRecipes(manifest, modules = {}) {
  const recipes = [];
  emitSite(manifest, {
    wing: wing => {
      if (!modules.wing) return null;
      const recipe = { module: modules.wing, params: { siteId: manifest.siteId, wingId: wing.id,
        seed: manifest.seed }, transform: wing.rootTransform };
      recipes.push(recipe); return recipe;
    },
    connector: connector => {
      if (!modules.connector) return null;
      const recipe = { module: modules.connector,
        params: { siteId: manifest.siteId, connectorId: connector.id, seed: manifest.seed } };
      recipes.push(recipe); return recipe;
    },
  });
  return recipes;
}
