// Executable connector-record fixture shared by unit tests and the native
// CastleConnectorFixture world. The canonical +30 degree case is the frozen
// 12x12 core / 14x8 hall handoff translated so the core mouth is at [12,0,6].

function add2(a, b) { return [a[0] + b[0], a[1] + b[1]]; }
function mul2(v, scale) { return [v[0] * scale, v[1] * scale]; }
function add3(point, vector) { return [point[0] + vector[0], point[1], point[2] + vector[1]]; }

function outwardNormal(a, b, side) {
  const dx = b[0] - a[0], dz = b[1] - a[1], length = Math.hypot(dx, dz);
  return side === 'south' ? [dz / length, -dx / length] : [-dz / length, dx / length];
}

function convexHull(points) {
  const sorted = points.map(point => [...point]).sort((a, b) => a[0] - b[0] || a[1] - b[1]);
  const cross = (a, b, c) => (b[0] - a[0]) * (c[1] - a[1]) -
    (b[1] - a[1]) * (c[0] - a[0]);
  const half = values => {
    const result = [];
    for (const point of values) {
      while (result.length >= 2 && cross(result.at(-2), result.at(-1), point) <= 1e-10)
        result.pop();
      result.push(point);
    }
    return result;
  };
  const lower = half(sorted), upper = half([...sorted].reverse());
  return lower.slice(0, -1).concat(upper.slice(0, -1));
}

function makeRecord(yawDeg, id) {
  const yaw = yawDeg * Math.PI / 180;
  const tangent = [Math.sin(yaw), Math.cos(yaw)];
  const hallOutward = [-Math.cos(yaw), Math.sin(yaw)];
  const coreCenter = [12, 6], hallCenter = [18, 6];
  const coreTangent = [0, 1], coreOutward = [1, 0];
  // The 45-degree miter needs a 2.0m mouth to retain the same 0.6m swept
  // capsule clearance that 1.6m mouths retain at the shallower angles.
  const halfWidth = Math.abs(yawDeg) === 45 ? 1.0 : 0.8, halfWall = 0.3;
  const coreInside = add2(coreCenter, mul2(coreOutward, -halfWall));
  const coreOutside = add2(coreCenter, mul2(coreOutward, halfWall));
  const hallInside = add2(hallCenter, mul2(hallOutward, -halfWall));
  const hallOutside = add2(hallCenter, mul2(hallOutward, halfWall));
  const segmentAt = (center, direction) => [
    add2(center, mul2(direction, -halfWidth)),
    add2(center, mul2(direction, halfWidth)),
  ];
  const coreInsideSegment = segmentAt(coreInside, coreTangent);
  const coreOutsideSegment = segmentAt(coreOutside, coreTangent);
  const hallInsideSegment = segmentAt(hallInside, tangent);
  const hallOutsideSegment = segmentAt(hallOutside, tangent);
  // The convex support hull spans all four wall-face mouth segments. Acute
  // connector ends remain fractional and asymmetric; none are grid-snapped.
  const clearPolygon = convexHull([
    ...coreInsideSegment, ...coreOutsideSegment,
    ...hallInsideSegment, ...hallOutsideSegment,
  ]);
  const lateral = clearPolygon.map((point, index) => [point,
    clearPolygon[(index + 1) % clearPolygon.length]])
    .filter(([a, b]) => Math.hypot(b[0] - a[0], b[1] - a[1]) > 3)
    .map(([a, b]) => a[0] <= b[0] ? [a, b] : [b, a])
    .sort((one, two) => (one[0][1] + one[1][1]) - (two[0][1] + two[1][1]));
  const [southSegment, northSegment] = lateral;
  const southNormal = outwardNormal(southSegment[0], southSegment[1], 'south');
  const northNormal = outwardNormal(northSegment[0], northSegment[1], 'north');
  const hallTrimConstant = hallOutward[0] * hallInside[0] + hallOutward[1] * hallInside[1];
  const trimPlanes = [
    { normal: [1, 0], offset: coreInside[0], keepSign: 1 },
    { normal: hallOutward, offset: hallTrimConstant, keepSign: 1 },
  ];
  const wallSpan = (side, a, b, normal) => {
    const length = Math.hypot(b[0] - a[0], b[1] - a[1]);
    const spanId = `connector:${id}:wall:${side}`;
    return {
      id: spanId, segment: [a, b],
      tangent: [(b[0] - a[0]) / length, (b[1] - a[1]) / length], normal,
      thickness: 0.6, courseOrigin: [a[0], 0, a[1]],
      cornerOwners: [`${spanId}:start`, `${spanId}:end`],
      jambOwners: [`connector:${id}:mouth:a:jambs`, `connector:${id}:mouth:b:jambs`],
      trimPlanes,
    };
  };
  const mouth = (wing, portalId, portalSourceId, inside, outside,
    centerSegment, insideSegment, outsideSegment, mouthTangent, outward, jambOwner) => ({
    wing, level: 'ground', portalId, portalSourceId,
    inside: [inside[0], 0, inside[1]], outside: [outside[0], 0, outside[1]],
    segment: centerSegment, insideSegment, outsideSegment,
    tangent: mouthTangent, outward, wallThickness: 0.6,
    hostModules: [`${wing}:ground:${wing === 'core' ? 'east' : 'west'}-wall`], jambOwner,
  });
  return {
    id, level: 'ground', baseY: 0, clearPolygon,
    mouths: [
      mouth('core', 'east-hall', 'east-hall', coreInside, coreOutside,
        segmentAt(coreCenter, coreTangent), coreInsideSegment, coreOutsideSegment,
        coreTangent, coreOutward, `connector:${id}:mouth:a:jambs`),
      mouth('hall', 'west-entry', 'west-entry', hallInside, hallOutside,
        segmentAt(hallCenter, tangent), hallInsideSegment, hallOutsideSegment,
        tangent, hallOutward, `connector:${id}:mouth:b:jambs`),
    ],
    wallSpans: [
      wallSpan('south', southSegment[0], southSegment[1], southNormal),
      wallSpan('north', northSegment[0], northSegment[1], northNormal),
    ],
    floor: { thickness: 0.22, material: 'castle.flagstone', owner: `connector:${id}:floor` },
    // The soffit and finite hip rafters need structural depth above passage.
    height: 3.9, clearHeight: 3.6,
    roof: { kind: 'low-hip', rise: 0.8, material: 'castle.roofTile', overhang: 0.18 },
    routeWaypoints: [
      [coreInside[0], 0, coreInside[1]], [coreOutside[0], 0, coreOutside[1]],
      [hallOutside[0], 0, hallOutside[1]], [hallInside[0], 0, hallInside[1]],
    ],
  };
}

export const CASTLE_CONNECTOR_FIXTURE = Object.freeze({
  schema: 'matter.castle-connectors/v1',
  core: Object.freeze({ size: [12, 12], eastMouth: [12, 0, 6] }),
  hall: Object.freeze({ size: [14, 8], westMouthLocal: [0, 0, 4],
    yawDeg: 30, origin: [16, 0, 2.5358983849], worldMouth: [18, 0, 6] }),
  records: Object.freeze([
    makeRecord(30, 'core-hall-30'),
    makeRecord(15, 'core-hall-15'),
    makeRecord(45, 'core-hall-45'),
    makeRecord(-30, 'core-hall-neg-30'),
  ]),
});

export function connectorFixtureRecords() {
  return [...CASTLE_CONNECTOR_FIXTURE.records].sort((a, b) => a.id.localeCompare(b.id));
}

export function connectorFixtureRecord(index) {
  const records = connectorFixtureRecords();
  if (!Number.isInteger(index) || index < 0 || index >= records.length)
    throw new RangeError(`connector fixture index ${index} is out of range`);
  return records[index];
}
