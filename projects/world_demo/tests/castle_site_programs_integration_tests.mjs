import assert from 'node:assert/strict';

await import('./castle_shared_lib_hooks.mjs');
const { compileSite, siteToJSON } = await import('../shared-lib/castle_site.js');
const { castleSiteProgram, CASTLE_SITE_NAMES } =
  await import('../shared-lib/castle_site_catalog.js');
const { connectorSolidVolumes, validateConnectorGeometry, validateConnectorRecords } =
  await import('../shared-lib/castle_connector_kit.js');
globalThis.defineMaterial = name => name;
const { castleSiteWorldDefinition } = await import('../shared-lib/castle_site_world.js');

const clone = value => structuredClone(value);
const EXPECTED_COURTYARDS = [1, 1, 2];

for (let variant = 0; variant < CASTLE_SITE_NAMES.length; ++variant) {
  const input = clone(castleSiteProgram(variant));
  const manifest = compileSite(input);
  assert.equal(manifest.siteId, CASTLE_SITE_NAMES[variant]);
  assert.equal(manifest.roomGraph.reachableRoomIds.length,
    manifest.roomGraph.nodes.filter(node => node.required).length,
    `${manifest.siteId} reaches every required room`);
  assert.equal(manifest.walkRoutes.length, manifest.roomGraph.reachableRoomIds.length);
  assert.equal(manifest.courtyards.length, EXPECTED_COURTYARDS[variant]);
  assert.ok(manifest.courtyards.every(courtyard =>
    manifest.roomGraph.reachableRoomIds.includes(courtyard.nodeId)));
  assert.equal(manifest.roomGraph.edges.filter(edge => edge.rooms.includes('outside')).length, 1,
    'the selected entry remains the only global outside edge');

  const records = validateConnectorRecords(manifest);
  assert.equal(records.valid, true, `${manifest.siteId}: ${records.errors.join('; ')}`);
  for (const connector of manifest.connectors) {
    assert.doesNotThrow(() => validateConnectorGeometry(connector),
      `${manifest.siteId}:${connector.id} has valid exact geometry`);
    assert.equal(connector.height, 3.6, 'authored enclosure height survives compilation');
    assert.equal(connector.clearHeight, 2.8,
      'physical arch clearance remains distinct from the taller enclosure');
    assert.ok(connector.wallSpans.every(span => span.height === connector.height));
    assert.ok(connectorSolidVolumes(connector).filter(volume => volume.kind === 'wall')
      .every(volume => volume.topY === connector.baseY + connector.height),
    'connector kit builds every wall solid to the authored enclosure height');
  }

  const reordered = clone(input);
  reordered.wings.reverse();
  reordered.connections.reverse();
  assert.equal(siteToJSON(compileSite(reordered)), siteToJSON(manifest),
    `${manifest.siteId} is stable under unordered input reversal`);
}

// Verify the visible footing against its physical collider in every world.
// A column-major translation silently produces a projective root matrix and
// a giant diagonal occluder in the native renderer.
for (const name of [...CASTLE_SITE_NAMES, 'gallery']) {
  const world = castleSiteWorldDefinition(name);
  for (const root of world.roots) if (root.transform)
    assert.deepEqual(root.transform.slice(12), [0, 0, 0, 1], `${name}:${root.module} affine row-major root`);
  const footing = world.roots.find(root => root.module === 'CastlePlinth');
  const body = world.entities.find(entity => entity.id === 'castle-site-footing').components;
  const matrix = footing.transform;
  const corners = [];
  for (const x of [-.5, .5]) for (const y of [-1.15, -.25]) for (const z of [-.5, .5])
    corners.push([0, 1, 2].map(row => matrix[row * 4] * x + matrix[row * 4 + 1] * y +
      matrix[row * 4 + 2] * z + matrix[row * 4 + 3]));
  for (let axis = 0; axis < 3; axis++) {
    const center = body.LocalTransform.translation[axis], half = body.BoxCollider.halfExtents[axis];
    assert.ok(Math.abs(Math.min(...corners.map(point => point[axis])) - (center - half)) < 1e-8);
    assert.ok(Math.abs(Math.max(...corners.map(point => point[axis])) - (center + half)) < 1e-8);
  }
}

console.log('castle_site_programs_integration_tests: all three decorated angled sites and gallery footing validate');
