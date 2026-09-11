import assert from 'node:assert/strict';

await import('./castle_shared_lib_hooks.mjs');
const { compileSite, siteToJSON } = await import('../shared-lib/castle_site.js');
const { castleSiteProgram, CASTLE_SITE_NAMES } =
  await import('../shared-lib/castle_site_catalog.js');
const { validateConnectorGeometry, validateConnectorRecords } =
  await import('../shared-lib/castle_connector_kit.js');

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
  }

  const reordered = clone(input);
  reordered.wings.reverse();
  reordered.connections.reverse();
  assert.equal(siteToJSON(compileSite(reordered)), siteToJSON(manifest),
    `${manifest.siteId} is stable under unordered input reversal`);
}

console.log('castle_site_programs_integration_tests: all three decorated angled sites compile and validate');
