import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { recipes, recipeById } from '../src/recipes/index.ts';
import type { RecipeDefinition } from '../src/recipes/common.ts';

assert.equal(recipes.length, 13);
assert.equal(recipeById.size, recipes.length);
function variant(recipe: RecipeDefinition, bound: 'default' | 'minimum' | 'maximum') {
  return Object.fromEntries(Object.entries(recipe.parameters).map(([key, p]) => [key, p[bound]]));
}
for (const recipe of recipes) {
  assert.equal(recipeById.get(recipe.id), recipe);
  assert.ok(recipe.name && recipe.description);
  const defaults = recipe.build();
  assert.deepEqual(defaults, recipe.build());
  assert.ok(Object.isFrozen(defaults));
  assert.ok(defaults.outputs.length > 0);
  assert.equal(defaults.asset_dependencies.length, 0);
  assert.equal(defaults.resources.length, 0);
  assert.throws(() => recipe.build({ unknown: 1 }));
  assert.throws(() => recipe.build({}, 'invalid'));
  for (const [key, p] of Object.entries(recipe.parameters)) {
    assert.ok(p.minimum <= p.default && p.default <= p.maximum && p.step > 0);
    assert.throws(() => recipe.build({ [key]: NaN }));
    assert.throws(() => recipe.build({ [key]: p.minimum - p.step }));
    assert.throws(() => recipe.build({ [key]: p.maximum + p.step }));
    if (p.type === 'integer') assert.throws(() => recipe.build({ [key]: p.default + 0.5 }));
  }
}
assert.notDeepEqual(recipeById.get('rock')!.build({}, '0000000000000001').nodes, recipeById.get('rock')!.build({}, '0000000000000002').nodes);
const native = process.argv[2];
assert.ok(native, 'Pass the native_recipes executable to test real native geometry.');
const failures: string[] = [];
for (const bound of ['default', 'minimum', 'maximum'] as const) {
  // Feed the actual SDK documents to a single native process. Geometry and
  // resolved inputs stay in memory; there are no generated mesh files/caches.
  const input = recipes.map(recipe => JSON.stringify(recipe.build(variant(recipe, bound)))).join('\n') + '\n';
  const result = spawnSync(native, [], { input, encoding: 'utf8', timeout: 10000, maxBuffer: 1024 * 1024 });
  process.stdout.write(`${bound} catalog:\n${result.stdout ?? ''}`);
  process.stderr.write(result.stderr ?? '');
  if (result.error || result.status !== 0) failures.push(`${bound}: ${result.error?.message ?? `native exit ${result.status}`}`);
}
// Prove that the topology oracle rejects valid wire documents with a missing
// face or reversed winding, rather than accepting every nonempty mesh.
for (const damage of ['open', 'inverted'] as const) {
  const broken = JSON.parse(JSON.stringify(recipeById.get('rock')!.build()));
  const positions: number[][] = broken.nodes[0].parameters.positions;
  if (damage === 'open') positions.splice(0, 3);
  else for (let i = 0; i < positions.length; i += 3) [positions[i], positions[i + 1]] = [positions[i + 1], positions[i]];
  const result = spawnSync(native, [], { input: JSON.stringify(broken) + '\n', encoding: 'utf8', timeout: 10000 });
  assert.ifError(result.error);
  assert.equal(result.status, 1);
  assert.match(result.stderr, damage === 'open' ? /open, nonmanifold/ : /inverted/);
}
assert.deepEqual(failures, [], 'Native catalog gates failed');
console.log('SDK controls, determinism and native default/minimum/maximum catalogs passed.');
