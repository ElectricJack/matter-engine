// Detailed geometry for polygonal castle vestibules. Connector records stay in
// world coordinates; Part parameters stay flat scalars. The site compiler owns
// placement, while this module owns every visible/collidable connector solid.

import { primitiveStock } from 'shared-lib/castle_stock';

export const CONNECTOR_MIN_CAPSULE_RADIUS = 0.4;
export const CONNECTOR_MIN_SIDE_CLEARANCE = 0.2;
export const CONNECTOR_MIN_PASSAGE_WIDTH =
  2 * (CONNECTOR_MIN_CAPSULE_RADIUS + CONNECTOR_MIN_SIDE_CLEARANCE);
export const CASTLE_CUT_STONE_VARIANT_COUNT = 2;

const EPS = 1e-8;

function fail(path, message) {
  throw new Error(`castle connector ${path}: ${message}`);
}

function finite(value, path) {
  if (typeof value !== 'number' || !Number.isFinite(value))
    fail(path, 'must be a finite number');
  return value;
}

function positive(value, path) {
  finite(value, path);
  if (value <= 0) fail(path, 'must be positive');
  return value;
}

function point2(value, path) {
  if (!Array.isArray(value) || value.length !== 2)
    fail(path, 'must be [x,z]');
  return [finite(value[0], `${path}[0]`), finite(value[1], `${path}[1]`)];
}

function point3(value, path) {
  if (!Array.isArray(value) || value.length !== 3)
    fail(path, 'must be [x,y,z]');
  return value.map((number, index) => finite(number, `${path}[${index}]`));
}

function requiredString(value, path) {
  if (typeof value !== 'string' || value.length === 0)
    fail(path, 'must be a non-empty string');
  return value;
}

function signedArea(polygon) {
  let twice = 0;
  for (let i = 0; i < polygon.length; ++i) {
    const a = polygon[i], b = polygon[(i + 1) % polygon.length];
    twice += a[0] * b[1] - b[0] * a[1];
  }
  return twice * 0.5;
}

function canonicalPolygon(input, path = 'clearPolygon') {
  if (!Array.isArray(input)) fail(path, 'must be an array');
  let polygon = input.map((point, index) => point2(point, `${path}[${index}]`));
  if (polygon.length > 3 && Math.hypot(
    polygon[0][0] - polygon.at(-1)[0], polygon[0][1] - polygon.at(-1)[1]) <= EPS)
    polygon.pop();
  if (polygon.length < 3 || polygon.length > 16)
    fail(path, 'must have 3..16 distinct vertices (32 collider points after extrusion)');
  const area = signedArea(polygon);
  if (Math.abs(area) <= EPS) fail(path, 'must have positive area');
  if (area < 0) polygon.reverse();
  polygon = polygon.filter((point, index) => {
    const previous = polygon[(index + polygon.length - 1) % polygon.length];
    return Math.hypot(point[0] - previous[0], point[1] - previous[1]) > EPS;
  });
  let changed = true;
  while (changed && polygon.length > 3) {
    changed = false;
    polygon = polygon.filter((point, index) => {
      const a = polygon[(index + polygon.length - 1) % polygon.length];
      const c = polygon[(index + 1) % polygon.length];
      const cross = (point[0] - a[0]) * (c[1] - point[1]) -
        (point[1] - a[1]) * (c[0] - point[0]);
      if (Math.abs(cross) <= 1e-9) { changed = true; return false; }
      return true;
    });
  }
  for (let i = 0; i < polygon.length; ++i) {
    const a = polygon[(i + polygon.length - 1) % polygon.length];
    const b = polygon[i], c = polygon[(i + 1) % polygon.length];
    const cross = (b[0] - a[0]) * (c[1] - b[1]) -
      (b[1] - a[1]) * (c[0] - b[0]);
    if (cross < -EPS) fail(path, 'must be convex and consistently wound');
  }
  let first = 0;
  for (let i = 1; i < polygon.length; ++i) {
    if (polygon[i][0] < polygon[first][0] - EPS ||
        (Math.abs(polygon[i][0] - polygon[first][0]) <= EPS &&
         polygon[i][1] < polygon[first][1] - EPS)) first = i;
  }
  return polygon.slice(first).concat(polygon.slice(0, first));
}

function convexHull(points, path) {
  const sorted = points.map((point, index) => point2(point, `${path}[${index}]`))
    .sort((a, b) => a[0] - b[0] || a[1] - b[1])
    .filter((point, index, values) => index === 0 ||
      Math.hypot(point[0] - values[index - 1][0], point[1] - values[index - 1][1]) > EPS);
  if (sorted.length < 3) fail(path, 'must contain at least three distinct points');
  const cross = (a, b, c) => (b[0] - a[0]) * (c[1] - a[1]) -
    (b[1] - a[1]) * (c[0] - a[0]);
  const half = values => {
    const result = [];
    for (const point of values) {
      while (result.length >= 2 && cross(result.at(-2), result.at(-1), point) <= EPS)
        result.pop();
      result.push(point);
    }
    return result;
  };
  return canonicalPolygon(half(sorted).slice(0, -1)
    .concat(half([...sorted].reverse()).slice(0, -1)), path);
}

function normalize2(value, path) {
  const v = point2(value, path);
  const length = Math.hypot(v[0], v[1]);
  if (length <= EPS) fail(path, 'must be non-zero');
  return [v[0] / length, v[1] / length];
}

function lineIntersection(a, ad, b, bd, path) {
  const cross = ad[0] * bd[1] - ad[1] * bd[0];
  if (Math.abs(cross) <= EPS) fail(path, 'has parallel adjacent support lines');
  const delta = [b[0] - a[0], b[1] - a[1]];
  const t = (delta[0] * bd[1] - delta[1] * bd[0]) / cross;
  return [a[0] + ad[0] * t, a[1] + ad[1] * t];
}

export function insetConvexPolygon(input, distance) {
  const polygon = canonicalPolygon(input);
  if (distance < 0 || !Number.isFinite(distance)) fail('clearance', 'must be non-negative');
  if (distance === 0) return polygon;
  let inset = polygon;
  for (let index = 0; index < polygon.length; ++index) {
    const a = polygon[index];
    const b = polygon[(index + 1) % polygon.length];
    const d = normalize2([b[0] - a[0], b[1] - a[1]], `edge[${index}]`);
    const normal = [-d[1], d[0]];
    inset = clipHalfPlane(inset, normal,
      normal[0] * a[0] + normal[1] * a[1] + distance, 1);
  }
  if (inset.length < 3 || Math.abs(signedArea(inset)) <= EPS)
    fail('clearPolygon', `cannot retain ${distance}m radial clearance`);
  return canonicalPolygon(inset, 'clearancePolygon');
}

function outsetConvexPolygon(input, distance) {
  const polygon = canonicalPolygon(input);
  if (distance <= EPS) return polygon;
  return polygon.map((point, index) => {
    const previous = polygon[(index + polygon.length - 1) % polygon.length];
    const next = polygon[(index + 1) % polygon.length];
    const previousDirection = normalize2(
      [point[0] - previous[0], point[1] - previous[1]], `roof.edge[${index}].previous`);
    const nextDirection = normalize2(
      [next[0] - point[0], next[1] - point[1]], `roof.edge[${index}].next`);
    const previousPoint = [point[0] + previousDirection[1] * distance,
      point[1] - previousDirection[0] * distance];
    const nextPoint = [point[0] + nextDirection[1] * distance,
      point[1] - nextDirection[0] * distance];
    return lineIntersection(previousPoint, previousDirection, nextPoint, nextDirection,
      `roof.vertex[${index}]`);
  });
}

function pointInsideConvex(point, polygon, tolerance = EPS) {
  for (let i = 0; i < polygon.length; ++i) {
    const a = polygon[i], b = polygon[(i + 1) % polygon.length];
    if ((b[0] - a[0]) * (point[1] - a[1]) -
        (b[1] - a[1]) * (point[0] - a[0]) < -tolerance) return false;
  }
  return true;
}

function normalizeMouth(input, index) {
  const path = `mouths[${index}]`;
  if (!input || typeof input !== 'object') fail(path, 'must be an object');
  const segmentInput = input.segment ?? input.centerSegment;
  const segment = [point2(segmentInput?.[0], `${path}.segment[0]`),
    point2(segmentInput?.[1], `${path}.segment[1]`)];
  const width = Math.hypot(segment[1][0] - segment[0][0], segment[1][1] - segment[0][1]);
  if (width + EPS < CONNECTOR_MIN_PASSAGE_WIDTH)
    fail(`${path}.segment`, `is ${width}m wide; requires ${CONNECTOR_MIN_PASSAGE_WIDTH}m`);
  const tangent = normalize2(input.tangent ?? input.tangentXZ, `${path}.tangent`);
  const outward = normalize2(input.outward ?? input.outwardXZ, `${path}.outward`);
  if (Math.abs(tangent[0] * outward[0] + tangent[1] * outward[1]) > 1e-5)
    fail(path, 'tangent and outward must be perpendicular');
  return {
    wing: requiredString(input.wing ?? input.wingId, `${path}.wing`),
    level: requiredString(input.level ?? input.levelId, `${path}.level`),
    portalId: requiredString(input.portalId, `${path}.portalId`),
    portalSourceId: input.portalSourceId === undefined ? input.portalId :
      requiredString(input.portalSourceId, `${path}.portalSourceId`),
    inside: point3(input.inside ?? input.insideThreshold, `${path}.inside`),
    outside: point3(input.outside ?? input.outsideThreshold, `${path}.outside`),
    insideSegment: (input.insideSegment ?? input.insideFaceSegment)?.map((point, pointIndex) =>
      point2(point, `${path}.insideSegment[${pointIndex}]`)) ?? segment,
    outsideSegment: (input.outsideSegment ?? input.outsideFaceSegment)?.map((point, pointIndex) =>
      point2(point, `${path}.outsideSegment[${pointIndex}]`)) ?? segment,
    segment, tangent, outward,
    wallThickness: positive(input.wallThickness, `${path}.wallThickness`),
    hostModules: Array.isArray(input.hostModules ?? input.hostModuleIds) ?
      (input.hostModules ?? input.hostModuleIds).map((value, hostIndex) =>
      requiredString(value, `${path}.hostModules[${hostIndex}]`)) :
      fail(`${path}.hostModules`, 'must be an array'),
    jambOwner: requiredString(input.jambOwner ?? input.jambOwnerId, `${path}.jambOwner`),
  };
}

function normalizeTrimPlane(input, path) {
  if (!input || typeof input !== 'object') fail(path, 'must be an object');
  return {
    normal: normalize2(input.normal ?? input.normalXZ, `${path}.normal`),
    offset: finite(input.offset ?? input.constant, `${path}.offset`),
    keepSign: input.keepSign === -1 || input.keep === 'le' ? -1 :
      input.keepSign === 1 || input.keep === 'ge' ? 1 :
      fail(`${path}.keepSign`, 'must be -1 or 1'),
  };
}

function normalizeSpan(input, index) {
  const path = `wallSpans[${index}]`;
  if (!input || typeof input !== 'object') fail(path, 'must be an object');
  const segmentInput = input.segment ?? input.clearFaceSegment ??
    (input.from && input.to ? [input.from, input.to] : undefined);
  const segment = [point2(segmentInput?.[0], `${path}.segment[0]`),
    point2(segmentInput?.[1], `${path}.segment[1]`)];
  const segmentLength = Math.hypot(segment[1][0] - segment[0][0],
    segment[1][1] - segment[0][1]);
  if (segmentLength < 0.18 - EPS)
    fail(`${path}.segment`, 'must retain at least 0.18m of masonry run');
  const tangent = normalize2(input.tangent ?? input.tangentXZ, `${path}.tangent`);
  const normal = normalize2(input.normal ?? input.outward ?? input.outwardXZ, `${path}.normal`);
  const along = normalize2([segment[1][0] - segment[0][0],
    segment[1][1] - segment[0][1]], `${path}.segment`);
  if (Math.abs(along[0] * tangent[1] - along[1] * tangent[0]) > 1e-5)
    fail(path, 'segment and tangent disagree');
  if (Math.abs(tangent[0] * normal[0] + tangent[1] * normal[1]) > 1e-5)
    fail(path, 'tangent and normal must be perpendicular');
  const ownership = (value, name) => {
    if (!Array.isArray(value) || value.length !== 2)
      fail(`${path}.${name}`, 'must contain start and end owners');
    return value.map((owner, ownerIndex) => requiredString(owner,
      `${path}.${name}[${ownerIndex}]`));
  };
  const thickness = positive(input.thickness, `${path}.thickness`);
  if (thickness < 0.16 - EPS)
    fail(`${path}.thickness`, 'must be at least 0.16m for exact masonry geometry');
  const trimPlanes = Array.isArray(input.trimPlanes) ? input.trimPlanes.map((plane, planeIndex) =>
    normalizeTrimPlane(plane, `${path}.trimPlanes[${planeIndex}]`)) :
    fail(`${path}.trimPlanes`, 'must be an array');
  for (const [planeIndex, plane] of trimPlanes.entries()) for (const endpoint of segment)
    if (plane.keepSign * (plane.normal[0] * endpoint[0] +
      plane.normal[1] * endpoint[1] - plane.offset) < -1e-6)
      fail(`${path}.trimPlanes[${planeIndex}]`, 'must preserve both declared span endpoints');
  const span = {
    id: requiredString(input.id, `${path}.id`), segment, tangent, normal,
    thickness,
    courseOrigin: point3(input.courseOrigin, `${path}.courseOrigin`),
    cornerOwners: ownership(input.cornerOwners ??
      [input.startJointOwnerId, input.endJointOwnerId], 'cornerOwners'),
    jambOwners: ownership(input.jambOwners ??
      [input.startJointOwnerId, input.endJointOwnerId], 'jambOwners'),
    trimPlanes,
  };
  const { front, back } = spanBedRanges(span);
  if (Math.min(front[1] - front[0], back[1] - back[0]) < 0.16 - EPS)
    fail(path, 'trimmed front and back masonry beds must retain at least 0.16m');
  return span;
}

function normalizedRecord(input) {
  if (!input || typeof input !== 'object') fail('record', 'must be an object');
  const id = requiredString(input.id, 'id');
  const level = requiredString(input.level ?? input.levelId, 'level');
  const baseY = finite(input.baseY, 'baseY');
  const clearPolygon = canonicalPolygon(input.clearPolygon);
  if (!Array.isArray(input.mouths) || input.mouths.length !== 2)
    fail('mouths', 'must contain exactly two portal mouths');
  const mouths = input.mouths.map(normalizeMouth);
  const portalIds = new Set(mouths.map(mouth => mouth.portalId));
  if (portalIds.size !== 2) fail('mouths', 'must refer to two distinct portals');
  if (!Array.isArray(input.wallSpans) || input.wallSpans.length < 2)
    fail('wallSpans', 'must contain finite boundary spans');
  const wallSpans = input.wallSpans.map(normalizeSpan)
    .sort((a, b) => a.id.localeCompare(b.id));
  const mouthJambOwners = new Set(mouths.map(mouth => mouth.jambOwner));
  for (const span of wallSpans) for (const owner of span.jambOwners)
    if (!mouthJambOwners.has(owner))
      fail(`wallSpans.${span.id}.jambOwners`, `${owner} is not owned by either connector mouth`);
  const floor = {
    thickness: positive(input.floor?.thickness, 'floor.thickness'),
    material: input.floor?.material ?? 'stone',
    owner: requiredString(input.floor?.owner ?? input.floor?.ownerId, 'floor.owner'),
  };
  const clearHeight = positive(input.clearHeight, 'clearHeight');
  if (clearHeight < 2.1) fail('clearHeight', 'must preserve at least 2.1m headroom');
  const height = positive(input.height ?? clearHeight, 'height');
  if (height + EPS < clearHeight) fail('height', 'must not be below clearHeight');
  const roof = {
    kind: requiredString(input.roof?.kind, 'roof.kind'),
    rise: positive(input.roof?.rise, 'roof.rise'),
    material: input.roof?.material ?? 'tile',
    overhang: input.roof?.overhang === undefined ? 0 : finite(input.roof.overhang, 'roof.overhang'),
  };
  if (roof.overhang < 0) fail('roof.overhang', 'must be non-negative');
  if (roof.kind !== 'low-hip') fail('roof.kind', 'only low-hip is supported');
  if (!Array.isArray(input.routeWaypoints) || input.routeWaypoints.length < 2)
    fail('routeWaypoints', 'must contain a continuous route');
  const routeWaypoints = input.routeWaypoints.map((point, index) =>
    point3(point, `routeWaypoints[${index}]`));
  for (let i = 0; i < routeWaypoints.length; ++i) {
    const point = routeWaypoints[i];
    if (!pointInsideConvex([point[0], point[2]], clearPolygon, 1e-6))
      fail(`routeWaypoints[${i}]`, 'must be supported by clearPolygon');
    if (Math.abs(point[1] - baseY) > 0.08)
      fail(`routeWaypoints[${i}]`, 'must stay on the connector floor elevation');
    for (const span of wallSpans) {
      const from = span.segment[0];
      const to = span.segment[1];
      const lengthSquared = (to[0] - from[0]) ** 2 + (to[1] - from[1]) ** 2;
      const projection = ((point[0] - from[0]) * (to[0] - from[0]) +
        (point[2] - from[1]) * (to[1] - from[1])) / lengthSquared;
      // Mouth centres legitimately pass beyond a finite side-wall end. The
      // portal segment, not an infinite continuation of that wall, bounds them.
      if (projection < -1e-6 || projection > 1 + 1e-6) continue;
      const inwardDistance = -((point[0] - from[0]) * span.normal[0] +
        (point[2] - from[1]) * span.normal[1]);
      if (inwardDistance + 1e-6 < CONNECTOR_MIN_CAPSULE_RADIUS + CONNECTOR_MIN_SIDE_CLEARANCE)
        fail(`routeWaypoints[${i}]`, `lacks 0.6m side clearance from wall ${span.id}`);
    }
  }
  for (let mouthIndex = 0; mouthIndex < mouths.length; ++mouthIndex) {
    const mouth = mouths[mouthIndex];
    for (const [name, threshold] of [['inside', mouth.inside], ['outside', mouth.outside]])
      if (!pointInsideConvex([threshold[0], threshold[2]], clearPolygon, 1e-6))
        fail(`mouths[${mouthIndex}].${name}`, 'threshold lacks connector floor support');
    for (const [name, segment] of [['insideSegment', mouth.insideSegment],
      ['outsideSegment', mouth.outsideSegment]]) for (const point of segment)
      if (!pointInsideConvex(point, clearPolygon, 1e-6))
        fail(`mouths[${mouthIndex}].${name}`, 'must lie on connector support polygon');
  }
  const clearancePolygon = insetConvexPolygon(clearPolygon,
    CONNECTOR_MIN_CAPSULE_RADIUS + CONNECTOR_MIN_SIDE_CLEARANCE);
  return { id, level, baseY, clearPolygon, mouths, wallSpans, floor,
    height, clearHeight, roof, routeWaypoints, clearancePolygon };
}

export function validateConnectorRecord(input) {
  try {
    const record = normalizedRecord(input);
    return { valid: true, errors: [], record, clearancePolygon: record.clearancePolygon };
  } catch (error) {
    return { valid: false, errors: [error instanceof Error ? error.message : String(error)] };
  }
}

export function validateConnectorRecords(siteOrRecords) {
  try {
    const records = connectorRecords(siteOrRecords).map(record => normalizedRecord(record));
    const ids = new Set(), portals = new Set(), ownedInterfaces = new Set();
    for (const record of records) {
      if (ids.has(record.id)) fail('records', `duplicate connector id ${record.id}`);
      ids.add(record.id);
      for (const mouth of record.mouths) {
        const portal = `${mouth.wing}/${mouth.level}/${mouth.portalId}`;
        if (portals.has(portal)) fail('records', `portal ${portal} is consumed more than once`);
        portals.add(portal);
      }
      for (const span of record.wallSpans) for (const [kind, owners] of [
        ['corner', span.cornerOwners], ['jamb', span.jambOwners],
      ]) for (let end = 0; end < 2; ++end) {
        const key = `${kind}:${span.id}:${end}:${owners[end]}`;
        if (ownedInterfaces.has(key)) fail('records', `duplicate interface ownership ${key}`);
        ownedInterfaces.add(key);
      }
    }
    return { valid: true, errors: [], records };
  } catch (error) {
    return { valid: false, errors: [error instanceof Error ? error.message : String(error)] };
  }
}

function clipHalfPlane(polygon, normal, offset, keepSign = 1) {
  const output = [];
  const inside = point => keepSign * (normal[0] * point[0] + normal[1] * point[1] - offset) >= -EPS;
  for (let i = 0; i < polygon.length; ++i) {
    const a = polygon[i], b = polygon[(i + 1) % polygon.length];
    const ai = inside(a), bi = inside(b);
    if (ai) output.push(a);
    if (ai !== bi) {
      const da = normal[0] * a[0] + normal[1] * a[1] - offset;
      const db = normal[0] * b[0] + normal[1] * b[1] - offset;
      const t = da / (da - db);
      output.push([a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t]);
    }
  }
  return output;
}

function spanFootprint(span) {
  const a = span.segment[0], b = span.segment[1], n = span.normal;
  let polygon = [
    [a[0], a[1]],
    [b[0], b[1]],
    [b[0] + n[0] * span.thickness, b[1] + n[1] * span.thickness],
    [a[0] + n[0] * span.thickness, a[1] + n[1] * span.thickness],
  ];
  for (const plane of span.trimPlanes)
    polygon = clipHalfPlane(polygon, plane.normal, plane.offset, plane.keepSign);
  if (polygon.length < 3 || Math.abs(signedArea(polygon)) <= EPS)
    fail(`wallSpans.${span.id}`, 'trim planes remove the complete wall span');
  return canonicalPolygon(polygon, `wallSpans.${span.id}.footprint`);
}

function convexIntersection(subject, clipper) {
  let result = subject;
  for (let i = 0; i < clipper.length && result.length; ++i) {
    const a = clipper[i], b = clipper[(i + 1) % clipper.length];
    const normal = [a[1] - b[1], b[0] - a[0]];
    result = clipHalfPlane(result, normal, normal[0] * a[0] + normal[1] * a[1], 1);
  }
  return result;
}

function prismPoints(polygon, bottomY, topY) {
  const points = [];
  for (const y of [bottomY, topY]) for (const point of polygon)
    points.push(point[0], y, point[1]);
  if (points.length > 96) fail('collider', 'exceeds ConvexHullCollider 32-point budget');
  return points;
}

export function connectorSolidVolumes(input, params = {}) {
  const record = normalizedRecord(input);
  const floorThickness = params.floorThickness ?? record.floor.thickness;
  const volumes = gridClippedPolygons(record.clearPolygon, params.flagSize ?? 0.9)
    .map((polygon, index) => ({
      id: `${record.id}:floor:${index}`, kind: 'floor', polygon,
      bottomY: record.baseY - floorThickness, topY: record.baseY,
      owner: record.floor.owner,
    }));
  for (const span of record.wallSpans) volumes.push({
    id: `${record.id}:wall:${span.id}`, kind: 'wall', polygon: spanFootprint(span),
    bottomY: record.baseY, topY: record.baseY + record.height,
    owner: span.cornerOwners.join('|'),
  });
  return volumes.map(volume => ({ ...volume,
    points: prismPoints(volume.polygon, volume.bottomY, volume.topY) }));
}

export function connectorClearanceVolumes(input) {
  const record = normalizedRecord(input);
  return [{
    id: `${record.id}:walking-clearance`, kind: 'capsule-route',
    polygon: record.clearancePolygon,
    bottomY: record.baseY,
    topY: record.baseY + record.clearHeight,
    radius: CONNECTOR_MIN_CAPSULE_RADIUS,
    sideClearance: CONNECTOR_MIN_SIDE_CLEARANCE,
    routeWaypoints: record.routeWaypoints,
  }];
}

export function validateConnectorGeometry(input, params = {}) {
  const record = normalizedRecord(input);
  const solids = connectorSolidVolumes(record, params);
  const walls = solids.filter(volume => volume.kind === 'wall');
  for (const wall of walls) {
    const overlap = convexIntersection(wall.polygon, record.clearPolygon);
    if (overlap.length >= 3 && Math.abs(signedArea(overlap)) > 1e-7)
      fail(`wallSpans.${wall.id}`, 'intrudes into the clear passage');
  }
  for (let i = 0; i < walls.length; ++i) for (let j = i + 1; j < walls.length; ++j) {
    const overlap = convexIntersection(walls[i].polygon, walls[j].polygon);
    if (overlap.length >= 3 && Math.abs(signedArea(overlap)) > 1e-7)
      fail('wallSpans', `${walls[i].id} and ${walls[j].id} have positive-volume duplicate masonry`);
  }
  return { valid: true, errors: [], record, solids,
    clearances: connectorClearanceVolumes(record) };
}

function orientedRectangle(polygon) {
  if (polygon.length !== 4) return null;
  const edges = polygon.map((point, index) => {
    const next = polygon[(index + 1) % 4];
    return [next[0] - point[0], next[1] - point[1]];
  });
  const lengths = edges.map(edge => Math.hypot(edge[0], edge[1]));
  if (lengths.some(length => length <= EPS)) return null;
  const dot = edges[0][0] * edges[1][0] + edges[0][1] * edges[1][1];
  const cross02 = edges[0][0] * edges[2][1] - edges[0][1] * edges[2][0];
  const cross13 = edges[1][0] * edges[3][1] - edges[1][1] * edges[3][0];
  if (Math.abs(dot) > 1e-6 * lengths[0] * lengths[1] ||
      Math.abs(cross02) > 1e-6 * lengths[0] * lengths[2] ||
      Math.abs(cross13) > 1e-6 * lengths[1] * lengths[3]) return null;
  const center = polygon.reduce((sum, point) =>
    [sum[0] + point[0] * 0.25, sum[1] + point[1] * 0.25], [0, 0]);
  return { center, length: lengths[0], depth: lengths[1],
    yaw: Math.atan2(-edges[0][1], edges[0][0]) };
}

export function connectorCollisionEntities(input, { prefix = 'castle-connector' } = {}) {
  return connectorSolidVolumes(input).map(volume => {
    const rectangle = orientedRectangle(volume.polygon);
    const components = {
      LocalTransform: rectangle ? {
        translation: [rectangle.center[0], (volume.bottomY + volume.topY) * 0.5,
          rectangle.center[1]],
        rotation: [0, Math.sin(rectangle.yaw * 0.5), 0, Math.cos(rectangle.yaw * 0.5)],
        scale: [1, 1, 1],
      } : { translation: [0, 0, 0], rotation: [0, 0, 0, 1], scale: [1, 1, 1] },
      RigidBody: { type: 'static', gravityScale: 1 },
    };
    if (rectangle) components.BoxCollider = {
      halfExtents: [rectangle.length * 0.5, (volume.topY - volume.bottomY) * 0.5,
        rectangle.depth * 0.5],
      density: 0, friction: 0.82, restitution: 0.02,
    };
    else components.ConvexHullCollider = {
      points: volume.points, density: 0, friction: 0.82, restitution: 0.02,
    };
    return {
      id: `${prefix}:${volume.id}`,
      name: `${volume.id} exact ${rectangle ? 'oriented box' : 'convex hull'} collider`,
      components,
    };
  });
}

function wrappedSeed(value) {
  const integer = Math.floor(Number.isFinite(value) ? value : 0);
  const seed = integer % CASTLE_CUT_STONE_VARIANT_COUNT;
  return seed < 0 ? seed + CASTLE_CUT_STONE_VARIANT_COUNT : seed;
}

export function connectorCutStoneParams(input = {}) {
  const geometry = value => Math.round(value * 1e6) / 1e6;
  const params = {
    seed: wrappedSeed(input.seed),
    height: geometry(Math.max(0.12, Number.isFinite(input.height) ? input.height : 0.28)),
    depth: geometry(Math.max(0.16, Number.isFinite(input.depth) ? input.depth : 0.42)),
    leftFront: geometry(Number.isFinite(input.leftFront) ? input.leftFront : -0.36),
    leftBack: geometry(Number.isFinite(input.leftBack) ? input.leftBack : -0.36),
    rightFront: geometry(Number.isFinite(input.rightFront) ? input.rightFront : 0.36),
    rightBack: geometry(Number.isFinite(input.rightBack) ? input.rightBack : 0.36),
    material: Math.max(0, Math.floor(Number.isFinite(input.material) ? input.material : 8)),
    detail: Math.max(0.5, Math.min(3, Number.isFinite(input.detail) ? input.detail : 1)),
  };
  if (Math.min(params.rightFront - params.leftFront,
    params.rightBack - params.leftBack) < 0.16)
    fail('cutStone', 'front and back beds must retain at least 0.16m');
  return params;
}

function cutPlane(part, point, inward, height, depth) {
  const length = Math.max(4, height + depth + 1);
  const outward = [-inward[0], -inward[1]];
  const center = [point[0] + outward[0] * length * 0.5, height * 0.5,
    point[1] + outward[1] * length * 0.5];
  const yaw = Math.atan2(-outward[1], outward[0]);
  part.pushMatrix();
  part.translate(center[0], center[1], center[2]);
  part.rotateY(yaw);
  part.box([0, 0, 0], [length * 0.5, height, length]);
  part.popMatrix();
  part.difference();
}

export function emitConnectorCutStone(part, input = {}) {
  const p = connectorCutStoneParams(input);
  const hz = p.depth * 0.5;
  const minX = Math.min(p.leftFront, p.leftBack);
  const maxX = Math.max(p.rightFront, p.rightBack);
  const centerX = (minX + maxX) * 0.5;
  const bevel = Math.min(0.035, p.height * 0.12, p.depth * 0.08);
  part.beginModifier();
  part.beginVoxels(Math.max(0.016, 0.026 / p.detail));
  part.fill(p.material);
  part.smoothing(Math.min(0.006, bevel * 0.2));
  part.box([centerX, p.height * 0.5, 0],
    [(maxX - minX) * 0.5 + bevel, p.height * 0.5, hz]);
  const leftDelta = p.leftBack - p.leftFront;
  const leftInward = normalize2([2 * hz, -leftDelta], 'cutStone.leftPlane');
  cutPlane(part, [p.leftFront, -hz], leftInward, p.height, p.depth);
  const rightDelta = p.rightBack - p.rightFront;
  const rightInward = normalize2([-2 * hz, rightDelta], 'cutStone.rightPlane');
  cutPlane(part, [p.rightFront, -hz], rightInward, p.height, p.depth);
  // Real recessed mortar beds and deterministic chisel losses remain inside
  // the voxel expression, so every clipped stone is more than a smooth wedge.
  const seedPhase = (p.seed + 1) * 0.61803398875;
  for (let mark = 0; mark < 2 + p.seed % 3; ++mark) {
    const t = ((seedPhase + mark * 0.38196601125) % 1 + 1) % 1;
    const z = -hz + bevel + t * (p.depth - 2 * bevel);
    const left = p.leftFront + (p.leftBack - p.leftFront) * ((z + hz) / p.depth);
    const right = p.rightFront + (p.rightBack - p.rightFront) * ((z + hz) / p.depth);
    const x = left + (right - left) * (0.24 + 0.52 * ((t * 1.7) % 1));
    part.capsule([x - 0.055, p.height * 0.48, z - 0.006],
      [x + 0.045, p.height * 0.63, z + 0.006], Math.max(0.012, bevel * 0.52));
    part.difference();
  }
  part.endVoxels();
  part.endModifier([]);
  return p;
}

export function connectorCutStoneChildVariants(base = {}, seeds) {
  const variants = seeds ?? Array.from({ length: CASTLE_CUT_STONE_VARIANT_COUNT }, (_, seed) => seed);
  if (!Array.isArray(variants)) fail('cutStoneChildVariants.seeds', 'must be an array');
  return variants.map(seed => ({ module: 'CastleConnectorCutStone',
    params: connectorCutStoneParams({ ...base, seed }) }));
}

function connectorRecords(siteOrRecords) {
  const records = Array.isArray(siteOrRecords) ? siteOrRecords : siteOrRecords?.connectors;
  if (!Array.isArray(records)) fail('siteOrRecords', 'must be connector records or {connectors}');
  return records;
}

function materialId(materials, key, fallback) {
  const value = materials?.[key];
  if (Array.isArray(value)) return value.length ? value[0] : fallback;
  return Number.isFinite(value) ? value : fallback;
}

function recipeParams(recordIndex, materials, detail) {
  return {
    connectorIndex: recordIndex,
    seed: wrappedSeed(recordIndex),
    detail: Math.max(0.5, Math.min(3, detail)),
    stoneMaterial: materialId(materials, 'stone', 8),
    mortarMaterial: materialId(materials, 'mortar', 9),
    floorMaterial: materialId(materials, 'floor', 8),
    tileMaterial: materialId(materials, 'tile', 10),
    timberMaterial: materialId(materials, 'timber', 14),
  };
}

export function connectorRecipes(siteOrRecords, {
  module = 'CastleConnector', materials = {}, detail = 1,
} = {}) {
  const records = connectorRecords(siteOrRecords).map(record => normalizedRecord(record))
    .sort((a, b) => a.id.localeCompare(b.id));
  return records.map((record, recordIndex) => ({
    module,
    params: recipeParams(recordIndex, materials, detail),
    recordId: record.id,
    expand: false,
    expanded: false,
    inlineGeometry: true,
  }));
}

// Native-safe split: inline exact polygons remain in an unexpanded mesh root;
// high-detail stones and rafters live in an expanded child-only root so their
// triangles stay instanced instead of flattening into one giant asset.
export function connectorLayerRecipes(siteOrRecords, {
  meshModule = 'CastleConnectorMesh',
  assemblyModule = 'CastleConnectorAssembly',
  materials = {}, detail = 1,
} = {}) {
  const records = connectorRecords(siteOrRecords).map(record => normalizedRecord(record))
    .sort((a, b) => a.id.localeCompare(b.id));
  return records.flatMap((record, recordIndex) => {
    const params = recipeParams(recordIndex, materials, detail);
    return [{
      module: meshModule, params: { ...params }, recordId: record.id,
      layer: 'mesh', expand: false, expanded: false, inlineGeometry: true,
    }, {
      module: assemblyModule, params: { ...params }, recordId: record.id,
      layer: 'children', expand: true, expanded: true, inlineGeometry: false,
    }];
  });
}

function emitPolygonPrism(part, polygon, bottomY, topY, material) {
  if (topY - bottomY <= EPS) return;
  part.fill(material);
  part.beginShape(SHAPE.polygon);
  // The extruder's +Y-path frame maps profile (u,v) to world (-v,-u).
  // Reverse the CCW XZ ring while applying that basis inverse so the emitted
  // prism exactly matches its world-coordinate collision polygon.
  for (let index = polygon.length - 1; index >= 0; --index) {
    const point = polygon[index];
    part.vertex(-point[1], -point[0]);
  }
  part.endShape();
  part.extrude([[0, bottomY, 0], [0, topY, 0]]);
}

function gridClippedPolygons(polygon, cellSize) {
  const minX = Math.min(...polygon.map(point => point[0]));
  const maxX = Math.max(...polygon.map(point => point[0]));
  const minZ = Math.min(...polygon.map(point => point[1]));
  const maxZ = Math.max(...polygon.map(point => point[1]));
  const startX = Math.floor(minX / cellSize) * cellSize;
  const startZ = Math.floor(minZ / cellSize) * cellSize;
  const pieces = [];
  for (let x = startX; x < maxX - EPS; x += cellSize) {
    for (let z = startZ; z < maxZ - EPS; z += cellSize) {
      let clipped = polygon;
      clipped = clipHalfPlane(clipped, [1, 0], x, 1);
      clipped = clipHalfPlane(clipped, [1, 0], Math.min(x + cellSize, maxX), -1);
      clipped = clipHalfPlane(clipped, [0, 1], z, 1);
      clipped = clipHalfPlane(clipped, [0, 1], Math.min(z + cellSize, maxZ), -1);
      if (clipped.length >= 3 && Math.abs(signedArea(clipped)) > EPS)
        pieces.push(canonicalPolygon(clipped, 'floorPiece'));
    }
  }
  return pieces;
}

function floorLayout(record, params) {
  return gridClippedPolygons(record.clearPolygon, params.flagSize ?? 0.9)
    .map((polygon, index) => ({
      id: `${record.id}:floor:${index}`, polygon,
      rectangle: orientedRectangle(polygon),
    }));
}

function floorPieceStock(piece, record, params, seed) {
  if (!piece.rectangle) return null;
  const rectangle = piece.rectangle;
  const stock = primitiveStock('CastleStone', {
    seed, length: rectangle.length, height: record.floor.thickness,
    depth: rectangle.depth, material: params.floorMaterial, detail: params.detail,
  });
  const desired = [rectangle.length, record.floor.thickness, rectangle.depth];
  if (desired.some((value, axis) =>
    Math.abs(stock.params[['length', 'height', 'depth'][axis]] * stock.scale[axis] - value) > EPS))
    return null;
  return stock;
}

function placeFloorChildren(part, record, params) {
  const placements = [];
  for (const piece of floorLayout(record, params)) {
    const stock = floorPieceStock(piece, record, params,
      params.seed + placements.length * 7);
    if (!stock) continue;
    const rectangle = piece.rectangle;
    const childParams = stock.params, scale = stock.scale;
    part.pushMatrix();
    part.translate(rectangle.center[0], record.baseY - record.floor.thickness,
      rectangle.center[1]);
    part.rotateY(rectangle.yaw);
    part.scale(...scale);
    part.placeChild('CastleStone', childParams);
    part.popMatrix();
    placements.push({ id: piece.id, polygon: piece.polygon,
      module: 'CastleStone', params: childParams, scale });
  }
  return placements;
}

function localCoordinates(point, origin, tangent, normal) {
  const delta = [point[0] - origin[0], point[1] - origin[1]];
  return [delta[0] * tangent[0] + delta[1] * tangent[1],
    delta[0] * normal[0] + delta[1] * normal[1]];
}

function sectionRangeAtZ(localPolygon, z) {
  const xs = [];
  for (let i = 0; i < localPolygon.length; ++i) {
    const a = localPolygon[i], b = localPolygon[(i + 1) % localPolygon.length];
    if (Math.abs(a[1] - z) <= 1e-7) xs.push(a[0]);
    if ((a[1] < z - EPS && b[1] > z + EPS) || (a[1] > z + EPS && b[1] < z - EPS)) {
      const t = (z - a[1]) / (b[1] - a[1]);
      xs.push(a[0] + (b[0] - a[0]) * t);
    }
  }
  if (xs.length < 2) fail('wallSpan', 'cannot resolve clipped miter section');
  return [Math.min(...xs), Math.max(...xs)];
}

function spanBedRanges(span) {
  const footprint = spanFootprint(span);
  const center = [
    (span.segment[0][0] + span.segment[1][0]) * 0.5 + span.normal[0] * span.thickness * 0.5,
    (span.segment[0][1] + span.segment[1][1]) * 0.5 + span.normal[1] * span.thickness * 0.5,
  ];
  const local = footprint.map(point => localCoordinates(point, center, span.tangent, span.normal));
  return {
    footprint, center, local,
    front: sectionRangeAtZ(local, -span.thickness * 0.5 + 1e-7),
    back: sectionRangeAtZ(local, span.thickness * 0.5 - 1e-7),
  };
}

function placedCutProfile(span, profile) {
  // rotateY maps local +X to tangent and local +Z to tangent's left normal.
  // South/right-handed spans therefore need their authored front/back beds
  // exchanged so the child profile still follows span.normal in world XZ.
  const placedNormal = [-span.tangent[1], span.tangent[0]];
  const sameNormal = placedNormal[0] * span.normal[0] +
    placedNormal[1] * span.normal[1] > 0;
  return sameNormal ? profile : {
    leftFront: profile.leftBack,
    leftBack: profile.leftFront,
    rightFront: profile.rightBack,
    rightBack: profile.rightFront,
  };
}

function placeWallCourses(part, record, span, params) {
  // A wallSpan is the compiler's explicit assignment of this complete finite
  // masonry run to the connector. Its endpoint owner IDs identify the one
  // connector-owned corner/jamb instances; records assigning them elsewhere
  // are rejected while normalizing the mouth ownership contract above.
  const { footprint, center, front, back } = spanBedRanges(span);
  const runMin = Math.min(front[0], back[0]), runMax = Math.max(front[1], back[1]);
  // Match castle_masonry's nominal 0.3m course grid at the shared base.
  const courseCount = Math.max(1, Math.round(record.height / 0.3));
  const courseHeight = record.height / courseCount;
  const courseOrigin = localCoordinates(
    [span.courseOrigin[0], span.courseOrigin[2]], center, span.tangent, span.normal)[0];
  const bricks = [], cutStones = [];
  for (let course = 0; course < courseCount; ++course) {
    const target = 0.70;
    const offset = courseOrigin + (course & 1 ? target * 0.5 : 0);
    const breaks = [runMin];
    const firstSafeBreak = Math.max(front[0], back[0]) + 0.18;
    const lastSafeBreak = Math.min(front[1], back[1]) - 0.18;
    for (let x = Math.floor((runMin - offset) / target + 1) * target + offset;
      x <= lastSafeBreak + EPS; x += target)
      if (x >= firstSafeBreak - EPS) breaks.push(x);
    breaks.push(runMax);
    for (let index = 0; index + 1 < breaks.length; ++index) {
      const a = breaks[index], b = breaks[index + 1];
      const middle = (a + b) * 0.5;
      const first = index === 0, last = index + 2 === breaks.length;
      const lf = (first ? front[0] : a) - middle;
      const lb = (first ? back[0] : a) - middle;
      const rf = (last ? front[1] : b) - middle;
      const rb = (last ? back[1] : b) - middle;
      const world = [center[0] + span.tangent[0] * middle,
        center[1] + span.tangent[1] * middle];
      part.pushMatrix();
      part.translate(world[0], record.baseY + course * courseHeight, world[1]);
      part.rotateY(Math.atan2(-span.tangent[1], span.tangent[0]));
      const seed = wrappedSeed(params.seed + course * 17 + index * 5);
      const isCut = Math.abs(lf - lb) > 1e-6 || Math.abs(rf - rb) > 1e-6;
      let childParams, childScale = [1, 1, 1];
      if (isCut) {
        const placedProfile = placedCutProfile(span, {
          leftFront: lf, leftBack: lb, rightFront: rf, rightBack: rb,
        });
        childParams = connectorCutStoneParams({ seed, height: courseHeight,
          depth: span.thickness, ...placedProfile,
          material: params.stoneMaterial, detail: params.detail });
        part.placeChild('CastleConnectorCutStone', childParams);
        cutStones.push({ spanId: span.id, course, index, world, params: childParams,
          yaw: Math.atan2(-span.tangent[1], span.tangent[0]),
          owners: first ? [span.cornerOwners[0], span.jambOwners[0]] :
            [span.cornerOwners[1], span.jambOwners[1]] });
      } else {
        const stock = primitiveStock('CastleStone', {
          seed, length: b - a, height: courseHeight, depth: span.thickness,
          material: params.stoneMaterial, detail: params.detail,
        });
        childParams = stock.params;
        childScale = stock.scale;
        part.scale(...childScale);
        part.placeChild('CastleStone', childParams);
      }
      part.popMatrix();
      bricks.push({ spanId: span.id, course, index, from: a, to: b, cut: isCut,
        module: isCut ? 'CastleConnectorCutStone' : 'CastleStone',
        params: childParams, scale: childScale });
    }
  }
  return { bricks, cutStones, footprint };
}

function emitTriangle(part, a, b, c, material) {
  part.fill(material);
  part.beginShape(SHAPE.triangles);
  part.vertex(...a); part.vertex(...b); part.vertex(...c);
  part.endShape();
}

function mix3(a, b, t) {
  return [a[0] + (b[0] - a[0]) * t,
    a[1] + (b[1] - a[1]) * t,
    a[2] + (b[2] - a[2]) * t];
}

function placeBeamBetween(part, a, b, params, seed) {
  const dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
  const horizontal = Math.hypot(dx, dz), length = Math.hypot(horizontal, dy);
  const yaw = Math.atan2(-dz, dx), pitch = Math.atan2(dy, horizontal);
  part.pushMatrix();
  part.translate((a[0] + b[0]) * 0.5, (a[1] + b[1]) * 0.5, (a[2] + b[2]) * 0.5);
  part.rotateY(yaw); part.rotateZ(pitch);
  const stock = primitiveStock('CastleBeam', {
    seed, length, width: 0.14, height: 0.18,
    material: params.timberMaterial, endMaterial: params.timberMaterial,
    ironMaterial: params.timberMaterial, joint: 1, strap: 0, detail: params.detail,
  });
  part.scale(...stock.scale);
  part.placeChild('CastleBeam', stock.params);
  part.popMatrix();
}

function emitRoof(part, record, params, { mesh = true, children = true } = {}) {
  const shellPolygon = convexHull([
    ...record.clearPolygon,
    ...record.wallSpans.flatMap(span => spanFootprint(span)),
  ], 'roof.shellPolygon');
  const polygon = outsetConvexPolygon(shellPolygon, record.roof.overhang);
  const eaveY = record.baseY + record.height;
  const centroid = polygon.reduce((sum, point) => [sum[0] + point[0], sum[1] + point[1]], [0, 0])
    .map(value => value / polygon.length);
  const apex = [centroid[0], eaveY + record.roof.rise, centroid[1]];
  const facets = [], tiles = [], rafters = [];
  for (let edge = 0; edge < polygon.length; ++edge) {
    const pa = polygon[edge], pb = polygon[(edge + 1) % polygon.length];
    const a = [pa[0], eaveY, pa[1]], b = [pb[0], eaveY, pb[1]];
    if (mesh) emitTriangle(part, b, a, apex, params.tileMaterial);
    facets.push([b, a, apex]);
    const rows = Math.max(2, Math.ceil(Math.hypot(apex[0] - (a[0] + b[0]) * 0.5,
      apex[2] - (a[2] + b[2]) * 0.5) / 0.42));
    for (let row = 0; row < rows; ++row) {
      const t0 = row / rows, t1 = (row + 1) / rows;
      const left0 = mix3(a, apex, t0), right0 = mix3(b, apex, t0);
      const left1 = mix3(a, apex, t1), right1 = mix3(b, apex, t1);
      const columns = Math.max(1, Math.ceil(Math.hypot(
        right0[0] - left0[0], right0[2] - left0[2]) / 0.36));
      for (let column = 0; column < columns; ++column) {
        const u0 = column / columns, u1 = (column + 1) / columns;
        const q0 = mix3(left0, right0, u0), q1 = mix3(left0, right0, u1);
        const q2 = mix3(left1, right1, Math.min(1, u1));
        const q3 = mix3(left1, right1, Math.min(1, u0));
        for (const q of [q0, q1, q2, q3]) q[1] += 0.025;
        if (mesh) {
          emitTriangle(part, q0, q2, q1, params.tileMaterial);
          if (Math.hypot(q2[0] - q3[0], q2[2] - q3[2]) > EPS)
            emitTriangle(part, q0, q3, q2, params.tileMaterial);
        }
        tiles.push([q0, q3, q2, q1]);
      }
    }
    if (children)
      placeBeamBetween(part, a, apex, params, wrappedSeed(params.seed + edge));
    rafters.push([a, apex]);
  }
  return { facets, tiles, rafters };
}

export function connectorChildVariants(input, params = {}) {
  const record = normalizedRecord(input);
  const p = normalizedEmitParams(params);
  const variants = [];
  const collector = {
    pushMatrix() {}, popMatrix() {}, translate() {}, scale() {}, rotateY() {}, rotateZ() {},
    placeChild(module, childParams) { variants.push({ module, params: childParams }); },
  };
  placeFloorChildren(collector, record, p);
  for (const span of record.wallSpans) placeWallCourses(collector, record, span, p);
  emitRoof(collector, record, p, { mesh: false, children: true });
  const seen = new Set();
  return variants.filter(variant => {
    const key = `${variant.module}:${JSON.stringify(variant.params)}`;
    if (seen.has(key)) return false;
    seen.add(key);
    return true;
  });
}

function normalizedEmitParams(params) {
  return {
    seed: wrappedSeed(params.seed), detail: Math.max(0.5, Math.min(3, params.detail ?? 1)),
    stoneMaterial: params.stoneMaterial ?? 8, mortarMaterial: params.mortarMaterial ?? 9,
    floorMaterial: params.floorMaterial ?? 8, tileMaterial: params.tileMaterial ?? 10,
    timberMaterial: params.timberMaterial ?? 14, flagSize: params.flagSize ?? 0.9,
  };
}

export function emitConnectorMesh(part, input, params = {}) {
  const record = normalizedRecord(input);
  const p = normalizedEmitParams(params);
  const floorPieces = floorLayout(record, p);
  const inlineFloorPieces = floorPieces.filter(piece =>
    !floorPieceStock(piece, record, p, p.seed));
  for (const piece of inlineFloorPieces)
    emitPolygonPrism(part, piece.polygon, record.baseY - record.floor.thickness,
      record.baseY, p.floorMaterial);
  for (const span of record.wallSpans) {
    const mortar = spanFootprint({ ...span, thickness: span.thickness * 0.86 });
    emitPolygonPrism(part, mortar, record.baseY + 0.012,
      record.baseY + record.height - 0.012, p.mortarMaterial);
  }
  const roof = emitRoof(part, record, p, { mesh: true, children: false });
  return {
    id: record.id, floorPieces: floorPieces.map(piece => piece.polygon),
    inlineFloorPieces: inlineFloorPieces.map(piece => piece.polygon),
    roofFacets: roof.facets, roofTiles: roof.tiles,
  };
}

export function emitConnectorChildren(part, input, params = {}) {
  const record = normalizedRecord(input);
  const p = normalizedEmitParams(params);
  const floorStones = placeFloorChildren(part, record, p);
  const wallBricks = [], cutStones = [];
  for (const span of record.wallSpans) {
    const result = placeWallCourses(part, record, span, p);
    wallBricks.push(...result.bricks); cutStones.push(...result.cutStones);
  }
  const roof = emitRoof(part, record, p, { mesh: false, children: true });
  return { id: record.id, floorStones, wallBricks, cutStones, rafters: roof.rafters };
}

export function emitConnector(part, input, params = {}) {
  const record = normalizedRecord(input);
  const mesh = emitConnectorMesh(part, record, params);
  const children = emitConnectorChildren(part, record, params);
  return {
    id: record.id,
    floorPieces: mesh.floorPieces, inlineFloorPieces: mesh.inlineFloorPieces,
    floorStones: children.floorStones,
    wallBricks: children.wallBricks, cutStones: children.cutStones,
    roofFacets: mesh.roofFacets, roofTiles: mesh.roofTiles, rafters: children.rafters,
    solidVolumes: connectorSolidVolumes(record),
    clearancePolygon: record.clearancePolygon,
  };
}
