import assert from 'node:assert/strict';
import fs from 'node:fs';
await import('./castle_shared_lib_hooks.mjs');
const { castleSitePlan, CASTLE_SITE_NAMES, CASTLE_SITE_SEEDS } =
  await import('../shared-lib/castle_site_programs.js');
const { decorateCastleWingPlan } = await import('../shared-lib/castle_wing_interiors.js');
const { compilePlan } = await import('../shared-lib/castle_plan.js');
const { masonryChildVariants, masonryOptions } = await import('../shared-lib/castle_masonry.js');
const source = fs.readFileSync(new URL('../shared-lib/castle_site_catalog.js', import.meta.url), 'utf8');
let moduleId = 0;
const fresh = () => import('data:text/javascript,' + encodeURIComponent(source + '\n// instance ' + moduleId++));
const clone = value => structuredClone(value);
const options = masonryOptions({});
let wingsChecked = 0;

for (let variant = 0; variant < CASTLE_SITE_NAMES.length; ++variant) {
  for (const seed of [CASTLE_SITE_SEEDS[variant], CASTLE_SITE_SEEDS[variant] + 17]) {
    const raw = castleSitePlan(CASTLE_SITE_NAMES[variant], seed);
    const rawBefore = clone(raw);
    // Exact pre-optimization behavior: decorate every wing before exposing any.
    const reference = clone(raw);
    for (const wing of reference.wings) wing.plan = decorateCastleWingPlan(wing.plan);
    const expected = reference.wings.map(wing => compilePlan(wing.plan));
    const expectedChildren = expected.map(manifest => masonryChildVariants(manifest, options));
    assert.deepEqual(raw, rawBefore, 'decoration/compilation must not mutate raw plans');

    for (const order of ['wing-first', 'site-first']) {
      const catalog = await fresh();
      if (order === 'site-first')
        assert.deepEqual(catalog.castleSiteProgram(variant, seed), reference);
      // Reverse order also proves that each selected wing is independently keyed.
      for (let index = expected.length - 1; index >= 0; --index) {
        const actual = catalog.castleSiteWingManifest(variant, index, seed);
        assert.deepEqual(actual, expected[index], `${variant}/${seed}/${index}/${order} manifest`);
        assert.deepEqual(masonryChildVariants(actual, options), expectedChildren[index],
          `${variant}/${seed}/${index}/${order} exact dependency list`);
        assert.strictEqual(catalog.castleSiteWingManifest(variant, index, seed), actual,
          'repeated lookup keeps existing memoized identity');
        wingsChecked++;
      }
      assert.deepEqual(catalog.castleSiteProgram(variant, seed), reference,
        'full-site output is identical after any selected-wing order');
      assert.deepEqual(catalog.castleSiteProgram(variant, seed + 1), (() => {
        const separate = castleSitePlan(CASTLE_SITE_NAMES[variant], seed + 1);
        for (const wing of separate.wings) wing.plan = decorateCastleWingPlan(wing.plan);
        return separate;
      })(), 'neighbor seed cannot share selected plan data');
    }
  }
}

const isolated = await fresh();
const manifest = isolated.castleSiteWingManifest(0, 0);
const manifestBefore = clone(manifest);
const program = isolated.castleSiteProgram(0);
program.wings[0].plan.fixtures.push({ id: 'external-mutation-test' });
assert.deepEqual(isolated.castleSiteWingManifest(0, 0), manifestBefore,
  'public plan mutation cannot alter cached manifest data through aliases');
const other = await fresh();
assert.ok(!other.castleSiteProgram(0).wings[0].plan.fixtures.some(f => f.id === 'external-mutation-test'),
  'module contexts have independent caches');
assert.throws(() => isolated.castleSiteWingManifest(-1, 0), /Invalid castle site variant/);
assert.throws(() => isolated.castleSiteWingManifest(0, 999), /Invalid castle wing index/);
console.log(`castle_site_catalog_lazy_tests: PASS (${wingsChecked} exact manifest/dependency comparisons)`);
