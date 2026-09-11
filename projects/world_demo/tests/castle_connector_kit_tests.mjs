import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
const primitiveSource = readFileSync(new URL(
  '../shared-lib/castle_primitives.js', import.meta.url), 'utf8');
const primitiveUrl = `data:text/javascript;base64,${Buffer.from(
  primitiveSource + '\n//# sourceURL=castle_primitives.test.mjs').toString('base64')}`;
const connectorSource = readFileSync(new URL(
  '../shared-lib/castle_connector_kit.js', import.meta.url), 'utf8').replace(
  "'shared-lib/castle_primitives'", JSON.stringify(primitiveUrl));
const connectorUrl = `data:text/javascript;base64,${Buffer.from(
  connectorSource + '\n//# sourceURL=castle_connector_kit.test.mjs').toString('base64')}`;
const {
  connectorChildVariants,
  connectorCollisionEntities,
  connectorRecipes,
  connectorSolidVolumes,
  emitConnector,
  emitConnectorCutStone,
  validateConnectorRecord,
  validateConnectorRecords,
} = await import(connectorUrl);

const fixture = JSON.parse(readFileSync(
  new URL('./fixtures/castle_connector_records.json', import.meta.url), 'utf8'));
const records = fixture.records;
const EPSILON = 1e-7;
const recipeOptions = Object.freeze({
  module: 'CastleConnector',
  materials: Object.freeze({ stone: 8, mortar: 9, floor: 10, timber: 14, tile: 12 }),
  detail: 1,
});
const params = Object.freeze({
  seed: 5, detail: 1, stoneMaterial: 8, mortarMaterial: 9,
  floorMaterial: 10, timberMaterial: 14, tileMaterial: 12,
});
globalThis.SHAPE = Object.freeze({ polygon: 3, triangles: 0 });

function clone(value) { return JSON.parse(JSON.stringify(value)); }

function polygonArea(polygon) {
  let twiceArea = 0;
  for (let index = 0; index < polygon.length; ++index) {
    const next = polygon[(index + 1) % polygon.length];
    twiceArea += polygon[index][0] * next[1] - next[0] * polygon[index][1];
  }
  return twiceArea * 0.5;
}

function cross(a, b, point) {
  return (b[0] - a[0]) * (point[1] - a[1]) -
    (b[1] - a[1]) * (point[0] - a[0]);
}

function insideConvex(polygon, point) {
  const sign = Math.sign(polygonArea(polygon));
  return polygon.every((vertex, index) =>
    sign * cross(vertex, polygon[(index + 1) % polygon.length], point) >= -EPSILON);
}

function normalized(vector) {
  return Math.abs(Math.hypot(...vector) - 1) < 1e-6;
}

function flatScalars(value) {
  return value && typeof value === 'object' && !Array.isArray(value) &&
    Object.values(value).every(item => typeof item === 'number' && Number.isFinite(item));
}

function canonicalPoints(points) {
  const triples = [];
  for (let index = 0; index < points.length; index += 3)
    triples.push(points.slice(index, index + 3).map(value => value.toFixed(8)).join(','));
  return triples.sort().join('|');
}

function colliderPoints(entity) {
  const { LocalTransform: transform, BoxCollider: box,
    ConvexHullCollider: hull } = entity.components;
  if (hull) return hull.points;
  assert.ok(box, entity.id + ' has a supported collider shape');
  assert.deepEqual(transform.scale, [1, 1, 1], entity.id + ' box uses unit transform scale');
  const [qx, qy, qz, qw] = transform.rotation;
  assert.ok(Math.abs(qx) < EPSILON && Math.abs(qz) < EPSILON,
    entity.id + ' box rotation is yaw-only');
  const cosine = 1 - 2 * qy * qy;
  const sine = 2 * qy * qw;
  const points = [];
  for (const xSign of [-1, 1]) for (const ySign of [-1, 1]) for (const zSign of [-1, 1]) {
    const x = xSign * box.halfExtents[0], y = ySign * box.halfExtents[1];
    const z = zSign * box.halfExtents[2];
    points.push(transform.translation[0] + cosine * x + sine * z,
      transform.translation[1] + y,
      transform.translation[2] - sine * x + cosine * z);
  }
  return points;
}

class RecordingPart {
  constructor() {
    this.ops = [];
    this.placements = [];
    this.matrixDepth = 0;
    this.voxelDepth = 0;
    this.modifierDepth = 0;
    this.lastGeometry = null;
    this.material = -1;
  }

  record(kind, args = []) {
    const numbers = [];
    const collect = value => {
      if (typeof value === 'number') numbers.push(value);
      else if (Array.isArray(value)) value.forEach(collect);
    };
    collect(args);
    assert.ok(numbers.every(Number.isFinite), kind + ' received non-finite geometry');
    const operation = { kind, args, material: this.material, csg: 'union' };
    this.ops.push(operation);
    return operation;
  }

  beginModifier() { ++this.modifierDepth; this.record('beginModifier'); }
  endModifier(stack) { assert.ok(this.modifierDepth-- > 0); this.record('endModifier', stack); }
  beginVoxels(spacing) { assert.ok(spacing > 0); ++this.voxelDepth; this.record('beginVoxels', [spacing]); }
  endVoxels() { assert.ok(this.voxelDepth-- > 0); this.record('endVoxels'); }
  fill(material) { this.material = material; this.record('fill', [material]); }
  smoothing(value) { this.record('smoothing', [value]); }
  box(center, halfExtents) { this.lastGeometry = this.record('box', [center, halfExtents]); }
  sphere(center, radius) { this.lastGeometry = this.record('sphere', [center, radius]); }
  capsule(a, b, radius) { this.lastGeometry = this.record('capsule', [a, b, radius]); }
  cylinder(a, b, radius) { this.lastGeometry = this.record('cylinder', [a, b, radius]); }
  difference() { assert.ok(this.lastGeometry); this.lastGeometry.csg = 'difference'; }
  beginShape(shape) { this.record('beginShape', [shape]); }
  vertex(...args) { this.record('vertex', args); }
  endShape() { this.record('endShape'); }
  extrude(path) { this.record('extrude', [path]); }
  pushMatrix() { ++this.matrixDepth; this.record('pushMatrix'); }
  popMatrix() { assert.ok(this.matrixDepth-- > 0); this.record('popMatrix'); }
  translate(...args) { this.record('translate', args); }
  scale(...args) { this.record('scale', args); }
  rotateX(value) { this.record('rotateX', [value]); }
  rotateY(value) { this.record('rotateY', [value]); }
  rotateZ(value) { this.record('rotateZ', [value]); }
  lookAt(target, up) { this.record('lookAt', [target, up]); }
  placeChild(module, childParams, opts) { this.placements.push({ module, params: childParams, opts }); }

  assertBalanced() {
    assert.equal(this.matrixDepth, 0, 'matrix stack balances');
    assert.equal(this.voxelDepth, 0, 'voxel session balances');
    assert.equal(this.modifierDepth, 0, 'modifier stack balances');
  }
}

assert.equal(fixture.schema, 'matter.castle-connectors/v1');
assert.deepEqual(records.map(record => record.id),
  ['core-hall-30', 'core-hall-15', 'core-hall-45', 'core-hall-neg-30']);

// The canonical fixture is the frozen-spec arrangement: its two portal-mouth
// centres are [12, 0, 6] and [18, 0, 6], and the moving hall uses yaw +30.
const canonical = records[0];
assert.deepEqual(Object.keys(canonical).sort(), [
  'baseY', 'clearHeight', 'clearPolygon', 'floor', 'id', 'level', 'mouths',
  'roof', 'routeWaypoints', 'wallSpans',
]);
assert.deepEqual(canonical.mouths[0].segment, [[11.7, 5.2], [11.7, 6.8]]);
assert.ok(Math.abs((canonical.mouths[1].inside[0] +
  canonical.mouths[1].outside[0]) * 0.5 - 18) < EPSILON);
assert.ok(Math.abs((canonical.mouths[1].inside[2] +
  canonical.mouths[1].outside[2]) * 0.5 - 6) < EPSILON);
assert.ok(Math.abs(Math.atan2(canonical.mouths[1].tangent[0],
  canonical.mouths[1].tangent[1]) * 180 / Math.PI - 30) < EPSILON);

for (const record of records) {
  assert.ok(polygonArea(record.clearPolygon) > 0, record.id + ' polygon is counter-clockwise');
  assert.equal(record.mouths.length, 2);
  assert.ok(record.mouths.every(mouth => normalized(mouth.tangent) && normalized(mouth.outward)));
  assert.ok(record.wallSpans.every(span => normalized(span.tangent) && normalized(span.normal)));
  const result = validateConnectorRecord(record);
  assert.equal(result.valid, true, record.id + ': ' + result.errors?.join('; '));
  assert.deepEqual(result.errors, []);
  assert.equal(result.record.id, record.id);
  assert.ok(result.clearancePolygon.length >= 3,
    record.id + ' publishes a capsule-centre clearance polygon');
  assert.ok(result.clearancePolygon.every(point =>
    insideConvex(record.clearPolygon, point)), record.id + ' clearance stays inside the throat');
  assert.ok(record.routeWaypoints.every(point => point[1] === record.baseY),
    record.id + ' route stays on the walkable floor elevation');
  for (const mouth of record.mouths) {
    assert.ok(insideConvex(record.clearPolygon, [mouth.inside[0], mouth.inside[2]]));
    assert.ok(mouth.insideSegment.every(point => insideConvex(record.clearPolygon, point)));
    assert.ok(mouth.outsideSegment.every(point => insideConvex(record.clearPolygon, point)));
  }
}

assert.deepEqual(records.slice(0, 3).map(record =>
  Math.round(Math.atan2(record.mouths[1].tangent[0], record.mouths[1].tangent[1]) * 180 / Math.PI)),
  [30, 15, 45]);
assert.equal(Math.round(Math.atan2(records[3].mouths[1].tangent[0],
  records[3].mouths[1].tangent[1]) * 180 / Math.PI), -30);

const validation = validateConnectorRecords([canonical]);
assert.equal(validation.valid, true);
assert.deepEqual(validation.errors, []);

const narrow = clone(canonical);
narrow.id = 'narrow';
narrow.mouths[1].segment = [[17.75, 5.56698729810778], [18.25, 6.43301270189222]];
assert.equal(validateConnectorRecord(narrow).valid, false);
assert.ok(validateConnectorRecord(narrow).errors.some(error =>
  /narrow|clearance|capsule|mouth|width/i.test(String(error))));

const nonConvex = clone(canonical);
nonConvex.id = 'non-convex';
nonConvex.clearPolygon = [canonical.clearPolygon[0], canonical.clearPolygon[2],
  canonical.clearPolygon[1], canonical.clearPolygon[3]];
assert.equal(validateConnectorRecord(nonConvex).valid, false);
assert.ok(validateConnectorRecord(nonConvex).errors.some(error => /convex|polygon/i.test(String(error))));

const missingOwner = clone(canonical);
missingOwner.id = 'missing-jamb-owner';
delete missingOwner.mouths[1].jambOwner;
assert.equal(validateConnectorRecord(missingOwner).valid, false);
assert.ok(validateConnectorRecord(missingOwner).errors.some(error => /jamb|owner/i.test(String(error))));

const reused = clone(canonical);
reused.id = 'same-sockets-twice';
const duplicateValidation = validateConnectorRecords([canonical, reused]);
assert.equal(duplicateValidation.valid, false);
assert.ok(duplicateValidation.errors.some(error => /reused|socket|portal/i.test(String(error))));

const volumes = connectorSolidVolumes(canonical, params);
assert.ok(volumes.length >= 3, 'floor and both masonry sides publish structural volumes');
assert.equal(new Set(volumes.map(volume => volume.id)).size, volumes.length);
for (const volume of volumes) {
  assert.ok(['string', 'number'].includes(typeof volume.owner), volume.id + ' has one owner');
  assert.ok(Array.isArray(volume.polygon) && volume.polygon.length >= 3);
  assert.ok(Number.isFinite(volume.bottomY) && Number.isFinite(volume.topY));
  assert.ok(volume.topY > volume.bottomY);
  assert.ok(Array.isArray(volume.points) && volume.points.length % 3 === 0);
  assert.ok(volume.points.length <= 32 * 3, volume.id + ' respects native hull vertex limit');
  assert.ok(volume.points.every(Number.isFinite));
}

const floorVolumes = volumes.filter(volume => volume.kind === 'floor');
assert.ok(floorVolumes.length > 0);
const floorArea = floorVolumes.reduce((sum, volume) => sum + Math.abs(polygonArea(volume.polygon)), 0);
assert.ok(Math.abs(floorArea - polygonArea(canonical.clearPolygon)) < 1e-6,
  'floor collider prisms support exactly the clipped connector polygon');
assert.ok(floorVolumes.every(volume => volume.polygon.every(point =>
  insideConvex(canonical.clearPolygon, point))), 'no floor piece grows outside the opening');

const collision = connectorCollisionEntities(canonical, { prefix: 'fixture:' });
assert.equal(collision.length, volumes.length, 'each declared solid has exactly one collider');
assert.equal(new Set(collision.map(entity => entity.id)).size, collision.length);
for (const entity of collision) {
  assert.ok(entity.id.startsWith('fixture:'));
  const points = colliderPoints(entity);
  assert.ok(Array.isArray(points));
  assert.equal(points.length % 3, 0);
  assert.ok(points.length <= 32 * 3);
  assert.ok(points.every(Number.isFinite));
}
assert.ok(collision.some(entity => entity.components.BoxCollider),
  'rectangular prisms reuse oriented boxes');
assert.ok(collision.some(entity => entity.components.ConvexHullCollider),
  'clipped boundary prisms use exact convex hulls');
assert.deepEqual(collision.map(entity => canonicalPoints(colliderPoints(entity))).sort(),
  volumes.map(volume => canonicalPoints(volume.points)).sort(),
  'collision hulls exactly match the validated solid-volume prisms');

const recipes = connectorRecipes([canonical], recipeOptions);
assert.ok(recipes.length > 0);
assert.ok(recipes.every(recipe => recipe.module === recipeOptions.module &&
  recipe.recordId === canonical.id && recipe.expanded === false &&
  recipe.inlineGeometry === true && flatScalars(recipe.params)));
assert.equal(JSON.stringify(connectorRecipes([canonical], recipeOptions)), JSON.stringify(recipes),
  'recipe generation is byte-for-byte deterministic');

const reordered = clone(canonical);
reordered.wallSpans.reverse();
assert.equal(JSON.stringify(connectorRecipes([reordered], recipeOptions)), JSON.stringify(recipes),
  'authored wall-span order does not affect recipe identity');

const distinct = clone(records[2]);
for (const mouth of distinct.mouths) {
  mouth.wing += '-45';
  mouth.portalId += '-45';
  mouth.hostModules = mouth.hostModules.map(id => id + '-45');
}
const orderedRecipes = connectorRecipes([canonical, distinct], recipeOptions);
assert.equal(JSON.stringify(connectorRecipes([distinct, canonical], recipeOptions)),
  JSON.stringify(orderedRecipes), 'connector-record input order does not affect recipe identity');

const childVariants = connectorChildVariants(canonical, params);
assert.ok(childVariants.length > 0, 'requires can declare the cut-stone bake catalogue');
assert.ok(childVariants.every(variant =>
  ['CastleConnectorCutStone', 'CastleStone', 'CastleBeam'].includes(variant.module) &&
  flatScalars(variant.params)), 'Part boundary parameters remain flat finite scalars');
assert.equal(new Set(childVariants.map(variant =>
  variant.module + ':' + JSON.stringify(variant.params))).size, childVariants.length,
  'miter/corner ownership never requests the same child twice');

const part = new RecordingPart();
const emitted = emitConnector(part, canonical, params);
part.assertBalanced();
assert.equal(emitted.id, canonical.id);
assert.ok(emitted.floorPieces.length > 0, 'emits individually clipped floor flags/planks');
assert.ok(emitted.wallBricks.length > 0, 'emits individually built masonry courses');
assert.ok(emitted.cutStones.length >= 4, 'both angled ends own asymmetric jamb/miter stones');
assert.ok(emitted.roofFacets.length >= 4, 'low hip is made from clipped facets');
assert.ok(emitted.roofTiles.length > emitted.roofFacets.length, 'roof facets carry individual tiles');
assert.ok(emitted.rafters.length > 0, 'roof includes structural rafters');
assert.deepEqual(emitted.solidVolumes, volumes);
assert.deepEqual(emitted.clearancePolygon, validateConnectorRecord(canonical).clearancePolygon);
assert.ok(part.placements.length > 0, 'detailed cut stone/timber pieces are real child placements');
assert.ok(part.ops.some(operation => operation.kind === 'extrude'),
  'floor and mortar geometry use real polygon prisms');

const cutPart = new RecordingPart();
const cutVariant = childVariants.find(variant => variant.module === 'CastleConnectorCutStone');
assert.ok(cutVariant, 'angled courses require a connector-owned cut-stone variant');
emitConnectorCutStone(cutPart, cutVariant.params);
cutPart.assertBalanced();
assert.ok(cutPart.ops.some(operation => operation.kind === 'beginVoxels'),
  'asymmetric cut stone uses voxel CSG rather than overlapping rotated boxes');
assert.ok(cutPart.ops.some(operation => operation.csg === 'difference'),
  'asymmetric front/back bed planes are genuinely clipped');

console.log('castle connector kit: PASS - exact 15/30/45/-30 polygon geometry, ownership and hulls');
