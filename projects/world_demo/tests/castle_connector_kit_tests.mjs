import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
const primitiveSource = readFileSync(new URL(
  '../shared-lib/castle_primitives.js', import.meta.url), 'utf8');
const primitiveUrl = `data:text/javascript;base64,${Buffer.from(
  primitiveSource + '\n//# sourceURL=castle_primitives.test.mjs').toString('base64')}`;
const stockSource = readFileSync(new URL(
  '../shared-lib/castle_stock.js', import.meta.url), 'utf8').replace(
  "'shared-lib/castle_primitives'", JSON.stringify(primitiveUrl));
const stockUrl = `data:text/javascript;base64,${Buffer.from(
  stockSource + '\n//# sourceURL=castle_stock.test.mjs').toString('base64')}`;
const connectorSource = readFileSync(new URL(
  '../shared-lib/castle_connector_kit.js', import.meta.url), 'utf8').replace(
  "'shared-lib/castle_stock'", JSON.stringify(stockUrl));
const connectorUrl = `data:text/javascript;base64,${Buffer.from(
  connectorSource + '\n//# sourceURL=castle_connector_kit.test.mjs').toString('base64')}`;
const {
  connectorChildVariants,
  connectorClearanceVolumes,
  connectorCollisionEntities,
  connectorLayerRecipes,
  connectorRecipes,
  connectorSolidVolumes,
  emitConnector,
  emitConnectorChildren,
  emitConnectorCutStone,
  emitConnectorMesh,
  validateConnectorGeometry,
  validateConnectorRecord,
  validateConnectorRecords,
} = await import(connectorUrl);
const fixtureModuleSource = readFileSync(new URL(
  '../shared-lib/castle_connector_fixture.js', import.meta.url), 'utf8');
const fixtureModule = await import(`data:text/javascript;base64,${Buffer.from(
  fixtureModuleSource + '\n//# sourceURL=castle_connector_fixture.test.mjs').toString('base64')}`);

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

function translatedRecord(input, dx, dz) {
  const record = clone(input);
  const point2 = point => { point[0] += dx; point[1] += dz; };
  const point3 = point => { point[0] += dx; point[2] += dz; };
  record.clearPolygon.forEach(point2);
  record.routeWaypoints.forEach(point3);
  for (const mouth of record.mouths) {
    point3(mouth.inside); point3(mouth.outside);
    mouth.segment.forEach(point2);
    mouth.insideSegment.forEach(point2);
    mouth.outsideSegment.forEach(point2);
  }
  for (const span of record.wallSpans) {
    span.segment.forEach(point2);
    point3(span.courseOrigin);
    for (const plane of span.trimPlanes)
      plane.offset += plane.normal[0] * dx + plane.normal[1] * dz;
  }
  return record;
}

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

function insideConvex(polygon, point, tolerance = EPSILON) {
  const sign = Math.sign(polygonArea(polygon));
  return polygon.every((vertex, index) =>
    sign * cross(vertex, polygon[(index + 1) % polygon.length], point) >= -tolerance);
}

function normalized(vector) {
  return Math.abs(Math.hypot(...vector) - 1) < 1e-6;
}

function triangleNormalY([a, b, c]) {
  return (b[2] - a[2]) * (c[0] - a[0]) -
    (b[0] - a[0]) * (c[2] - a[2]);
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
assert.deepEqual(fixtureModule.CASTLE_CONNECTOR_FIXTURE.hall.origin,
  [16, 0, 2.5358983849]);
assert.deepEqual(records, fixtureModule.CASTLE_CONNECTOR_FIXTURE.records,
  'JSON contract fixture and executable/native fixture are byte-exact records');
for (const record of fixtureModule.CASTLE_CONNECTOR_FIXTURE.records) {
  assert.equal(validateConnectorRecord(record).valid, true,
    record.id + ' executable native fixture record validates');
  assert.equal(validateConnectorGeometry(record).valid, true,
    record.id + ' executable native fixture geometry validates');
}

// The canonical fixture is the frozen-spec arrangement: its two portal-mouth
// centres are [12, 0, 6] and [18, 0, 6], and the moving hall uses yaw +30.
const canonical = records[0];
assert.deepEqual(Object.keys(canonical).sort(), [
  'baseY', 'clearHeight', 'clearPolygon', 'floor', 'id', 'level', 'mouths',
  'roof', 'routeWaypoints', 'wallSpans',
]);
assert.deepEqual(canonical.mouths[0].segment, [[12, 5.2], [12, 6.8]]);
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
  assert.equal(validateConnectorGeometry(record).valid, true,
    record.id + ' concrete JSON fixture geometry validates');
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

const tallerEnclosure = clone(canonical);
tallerEnclosure.id = 'taller-enclosure';
tallerEnclosure.height = 4;
tallerEnclosure.clearHeight = 3.2;
const tallerSolids = connectorSolidVolumes(tallerEnclosure, params);
assert.ok(tallerSolids.filter(volume => volume.kind === 'wall').every(volume =>
  Math.abs(volume.topY - 4) < EPSILON), 'wall solids rise to optional enclosure height');
assert.equal(connectorClearanceVolumes(tallerEnclosure)[0].topY, 3.2,
  'walk clearance remains capped by physical portal headroom');
const tallerMesh = emitConnectorMesh(new RecordingPart(), tallerEnclosure, params);
assert.ok(tallerMesh.roofFacets.every(facet =>
  Math.abs(facet[0][1] - 4) < EPSILON && Math.abs(facet[1][1] - 4) < EPSILON),
  'roof eaves use enclosure height without overstating route headroom');

const missingOwner = clone(canonical);
missingOwner.id = 'missing-jamb-owner';
delete missingOwner.mouths[1].jambOwner;
assert.equal(validateConnectorRecord(missingOwner).valid, false);
assert.ok(validateConnectorRecord(missingOwner).errors.some(error => /jamb|owner/i.test(String(error))));

const foreignJamb = clone(canonical);
foreignJamb.id = 'foreign-jamb-owner';
foreignJamb.wallSpans[0].jambOwners[0] = 'wing:core:owns-this-jamb';
assert.equal(validateConnectorRecord(foreignJamb).valid, false,
  'connector refuses to emit an endpoint assigned to a foreign owner');

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
assert.equal(JSON.stringify(connectorSolidVolumes(reordered, params)), JSON.stringify(volumes),
  'authored wall-span order does not affect solid-volume identity');
assert.equal(JSON.stringify(connectorChildVariants(reordered, params)),
  JSON.stringify(connectorChildVariants(canonical, params)),
  'authored wall-span order does not affect child-variant identity');

const distinct = clone(records[2]);
for (const mouth of distinct.mouths) {
  mouth.wing += '-45';
  mouth.portalId += '-45';
  mouth.hostModules = mouth.hostModules.map(id => id + '-45');
}
const orderedRecipes = connectorRecipes([canonical, distinct], recipeOptions);
assert.equal(JSON.stringify(connectorRecipes([distinct, canonical], recipeOptions)),
  JSON.stringify(orderedRecipes), 'connector-record input order does not affect recipe identity');

const layers = connectorLayerRecipes([canonical], {
  meshModule: 'CastleConnectorFixtureMesh',
  assemblyModule: 'CastleConnectorFixtureAssembly',
  materials: recipeOptions.materials,
  detail: recipeOptions.detail,
});
assert.equal(layers.length, 2, 'each connector publishes an inline and child-only layer');
assert.deepEqual(layers.map(layer => [layer.layer, layer.expand, layer.inlineGeometry]), [
  ['mesh', false, true], ['children', true, false],
]);
assert.ok(layers.every(layer => flatScalars(layer.params)));

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
const firstShape = part.ops.findIndex(operation =>
  operation.kind === 'beginShape' && operation.args[0] === SHAPE.polygon);
const firstShapeEnd = part.ops.findIndex((operation, index) =>
  index > firstShape && operation.kind === 'endShape');
const emittedProfileXZ = part.ops.slice(firstShape + 1, firstShapeEnd)
  .filter(operation => operation.kind === 'vertex')
  .map(operation => [-operation.args[1], -operation.args[0]])
  .map(point => point.map(value => value.toFixed(8)).join(','))
  .sort();
const expectedFloorXZ = emitted.floorPieces[0]
  .map(point => point.map(value => value.toFixed(8)).join(','))
  .sort();
assert.deepEqual(emittedProfileXZ, expectedFloorXZ,
  'vertical extrusion basis inverse preserves exact world XZ floor polygon');

const meshPart = new RecordingPart();
const meshOnly = emitConnectorMesh(meshPart, canonical, params);
meshPart.assertBalanced();
assert.equal(meshPart.placements.length, 0,
  'unexpanded mesh layer contains no child placements');
assert.ok(meshPart.ops.some(operation => operation.kind === 'extrude'));
assert.ok(meshOnly.inlineFloorPieces.length > 0,
  'fractional boundary flags remain exact inline polygons');
assert.ok(meshOnly.roofFacets.flatMap(facet => facet.slice(0, 2)).some(point =>
  !insideConvex(canonical.clearPolygon, [point[0], point[2]])),
  'roof facets honor the authored overhang beyond the clear support polygon');
const roofEavePolygon = meshOnly.roofFacets.map(facet =>
  [facet[1][0], facet[1][2]]);
assert.ok(volumes.filter(volume => volume.kind === 'wall').every(volume =>
  volume.polygon.every(point => insideConvex(roofEavePolygon, point))),
  'roof eaves cover the complete masonry shell before adding overhang');
assert.ok(meshOnly.roofFacets.every(facet => triangleNormalY(facet) > EPSILON),
  'hip facets are wound upward');
assert.ok(meshOnly.roofTiles.every(tile => Math.max(
  triangleNormalY(tile.slice(0, 3)),
  triangleNormalY([tile[0], tile[2], tile[3]])) > EPSILON),
  'individual roof tiles are wound upward');

const childPart = new RecordingPart();
const childrenOnly = emitConnectorChildren(childPart, canonical, params);
childPart.assertBalanced();
assert.equal(childPart.ops.some(operation => operation.kind === 'beginShape'), false,
  'expanded assembly layer contains no inline mesh');
assert.ok(childPart.placements.some(placement => placement.module === 'CastleStone'),
  'rectangular interior flags reuse CastleStone children');
assert.ok(childrenOnly.cutStones.every(stone =>
  Math.abs(stone.params.height - 0.3) < EPSILON),
  'connector courses match castle_masonry nominal 0.3m cadence');
const placementVariants = new Set(childPart.placements.map(placement =>
  placement.module + ':' + JSON.stringify(placement.params)));
assert.deepEqual(new Set(childVariants.map(variant =>
  variant.module + ':' + JSON.stringify(variant.params))), placementVariants,
  'declared requires exactly cover child-only assembly placements');

const translated = translatedRecord(canonical, 0.75, 0);
const translatedParams = { ...params };
const translatedMesh = emitConnectorMesh(new RecordingPart(), translated, translatedParams);
const translatedChildPart = new RecordingPart();
const translatedChildren = emitConnectorChildren(
  translatedChildPart, translated, translatedParams);
assert.ok(translatedMesh.inlineFloorPieces.length > 0,
  'fractional non-rectangular flags remain exact inline geometry');
assert.ok(translatedMesh.inlineFloorPieces.some(polygon =>
  Math.abs(polygonArea(polygon)) < 0.18 * 0.9),
  'subminimum rectangular slivers remain inline instead of clamping stock dimensions');
for (const stone of translatedChildren.floorStones) {
  assert.ok(Math.abs(stone.params.length * stone.scale[0] *
    stone.params.depth * stone.scale[2] -
    Math.abs(polygonArea(stone.polygon))) < 1e-6,
  stone.id + ' scaled stock floor child preserves its exact plan area');
  assert.ok(Math.abs(stone.params.height * stone.scale[1] -
    translated.floor.thickness) < EPSILON,
  stone.id + ' scaled stock floor child preserves exact floor thickness');
}
const resizedParams = { ...params, flagSize: 0.75 };
const resizedPart = new RecordingPart();
emitConnectorChildren(resizedPart, translated, resizedParams);
const resizedPlacementVariants = new Set(resizedPart.placements.map(placement =>
  placement.module + ':' + JSON.stringify(placement.params)));
assert.deepEqual(new Set(connectorChildVariants(translated, resizedParams).map(variant =>
  variant.module + ':' + JSON.stringify(variant.params))), resizedPlacementVariants,
  'non-default flagSize child catalogue exactly matches emitted placements');

const ordinaryVariants = childVariants.filter(variant => variant.module === 'CastleStone');
assert.ok(ordinaryVariants.length <= 10,
  'five material palettes use at most two ordinary stock stones each');
assert.ok(ordinaryVariants.every(variant => variant.params.length === 0.72 &&
  variant.params.height === 0.28 && variant.params.depth === 0.42 &&
  variant.params.seed < 2), 'ordinary stone variants use shared canonical size and two seeds');
const beamVariants = childVariants.filter(variant => variant.module === 'CastleBeam');
assert.ok(beamVariants.length <= 2 && beamVariants.every(variant =>
  variant.params.length === 4 && variant.params.seed < 2),
  'rafters reuse the shared two-seed canonical beam stock');
for (const brick of childrenOnly.wallBricks.filter(brick => !brick.cut)) {
  assert.ok(Math.abs(brick.params.length * brick.scale[0] -
    (brick.to - brick.from)) < EPSILON,
  brick.spanId + ' stock brick scale preserves exact run length');
  assert.ok(Math.abs(brick.params.height * brick.scale[1] - 0.3) < EPSILON,
    brick.spanId + ' stock brick scale preserves course height');
}

for (const record of records) {
  const emittedRecord = emitConnectorChildren(new RecordingPart(), record, params);
  const wallVolumes = connectorSolidVolumes(record, params)
    .filter(volume => volume.kind === 'wall');
  for (const stone of emittedRecord.cutStones) {
    const wall = wallVolumes.find(volume => volume.id.endsWith(`wall:${stone.spanId}`));
    assert.ok(wall, stone.spanId + ' has a declared solid footprint');
    const cosine = Math.cos(stone.yaw), sine = Math.sin(stone.yaw);
    const p = stone.params, halfDepth = p.depth * 0.5;
    for (const [x, z] of [
      [p.leftFront, -halfDepth], [p.rightFront, -halfDepth],
      [p.rightBack, halfDepth], [p.leftBack, halfDepth],
    ]) {
      const worldPoint = [stone.world[0] + cosine * x + sine * z,
        stone.world[1] - sine * x + cosine * z];
      assert.ok(insideConvex(wall.polygon, worldPoint, 2e-6),
        `${record.id} ${stone.spanId} cut stone stays inside its wall solid`);
    }
  }
}

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
