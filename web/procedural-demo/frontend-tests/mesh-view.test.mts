import assert from 'node:assert/strict';
import { test } from 'node:test';
import { Mesh, Vector3 } from 'three';
import { identity, translation, RecipeBuilder } from '../../../packages/matter-authoring/index.mjs';
import { createMeshView } from '../src/mesh-view.ts';
import type { EvaluationResult } from '../src/evaluation.ts';

test('one prototype is shared by two instances with row-major placement; unused prototype stays invisible', () => {
  const builder = new RecipeBuilder({ source: { id: 'view.test', uri: 'test://view', revision: '1' }, seed: 1n });
  builder.material('clay');
  builder.output(builder.box('fixture', [1, 1, 1], { materials: ['clay'] }));
  const mesh = { id: 'shared', positions: Float32Array.of(0, 0, 0, 1, 0, 0, 0, 1, 0), normals: new Float32Array(), uvs: new Float32Array(), indices: Uint32Array.of(0, 1, 2), materialIds: Uint32Array.of(0), partIds: Uint32Array.of(0), groups: [{ start: 0, count: 3, materialIndex: 0 }], bounds: { min: [0, 0, 0], max: [1, 1, 0] } };
  const result = {
    meshes: [mesh, { ...mesh, id: 'unused' }], instances: [
      { id: 'one', partId: 'shared', meshId: 'shared', transform: identity() },
      { id: 'two', partId: 'shared', meshId: 'shared', transform: translation(3, 2, 1) },
    ], materials: builder.build().materials, parts: ['shared'], lods: [], diagnostics: [],
    bounds: mesh.bounds, measurements: { elapsedMs: 1, wasmHeapBytes: 0, transferredBytes: 0 },
  } as EvaluationResult;
  const view = createMeshView(result);
  assert.equal(view.root.children.length, 2); assert.equal(view.triangles, 2);
  const [one, two] = view.root.children as Mesh[];
  assert.equal(one.geometry, two.geometry);
  assert.deepEqual(new Vector3().applyMatrix4(two.matrix).toArray(), [3, 2, 1]);
  assert.equal(two.geometry.userData.partIds, mesh.partIds);
  const original = one.material;
  view.setDisplay(true, true); assert.notEqual(one.material, original);
  view.setDisplay(false, false); assert.equal(one.material, original);
  let releases = 0; one.geometry.addEventListener('dispose', () => releases++);
  view.dispose(); view.dispose(); assert.equal(releases, 1); assert.equal(view.root.children.length, 0);
});
