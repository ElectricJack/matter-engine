import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import * as sdk from '../../../../../packages/matter-authoring/index.mjs';
import * as contracts from '../../../../../libs/MatterContracts/examples/contracts.mjs';
import * as generators from './generators.mjs';
import {extrudedProp, orderedCsg, repeatedAssembly} from '../../../../../packages/matter-authoring/examples/recipes.mjs';
import {expectedProp, expectedRepeated, expectedCsg, rngVectors} from '../../../../../packages/matter-authoring/tests/expected.mjs';
const binary = process.argv[2];
assert(binary, 'native test executable is required');
const services = sdk.contractServices(contracts, async bytes => new Uint8Array(createHash('sha256').update(bytes).digest()));
// These functions have no private module state. Serialize their actual bodies
// into the supplied snapshot rather than opening source files as test data.
const fixtureSource = Object.values(generators).map(fn => `export ${fn.toString()}`).join('\n');
const fixtures = [];
function native(source, helper, seed, parameters, legacy = false) {
  const input = {entry:'fixture/main.mjs', seed, parameters, modules:[
    {id:'fixture/main.mjs',uri:'fixture://native',dependencies:['fixture/generators.mjs'],source},
    {id:'fixture/generators.mjs',uri:'fixture://generators',dependencies:[],source:helper}]};
  if (legacy) input.legacy_voxels = true;
  return input;
}
for (const name of ['legacyProp','legacyCsg','legacyChildren','seeded','digest']) {
  const seeds = name === 'seeded' ? ['0000000000000000','0000000000000001','0123456789abcdef','ffffffffffffffff'] : ['0123456789abcdef'];
  for (const seed of seeds) {
    const parameters = {label:'héllo 🌲',width:0.1};
    const context = {source:{id:'fixture/main.mjs',uri:'fixture://native',revision:'1',content:null,legacy_hashes:[]},
      parameters, seed:BigInt('0x'+seed), services,
      supportedCapabilities:[...sdk.DEFAULT_CAPABILITIES, ...(name==='legacyCsg' ? [sdk.LEGACY_VOXELS] : [])]};
    const expected = await generators[name](sdk, context);
    const source = `import * as sdk from 'matter:authoring'; import {${name}} from './generators.mjs'; export default ctx => ${name}(sdk,ctx);`;
    const input = native(source, fixtureSource, seed, parameters, name==='legacyCsg');
    if (name === 'seeded') {
      const vector = rngVectors.vectors.find(v => v.seed === seed);
      assert.deepEqual(expected.metadata, {words:vector.words,units:vector.units});
    }
    fixtures.push({name:`HOST-SDK-${name}-${seed}`, input, expected});
  }
}
// Independently frozen SDK/core fixtures, not only two runs of one generator.
const examples = `import {RecipeBuilder,nodeInput,parameters,identity,multiply,scale,translation} from 'matter:authoring';\n`
  + [extrudedProp, orderedCsg, repeatedAssembly].map(fn => `export ${fn.toString()}`).join('\n');
for (const [name, expected] of [['extrudedProp',expectedProp],['orderedCsg',expectedCsg],['repeatedAssembly',expectedRepeated]]) {
  const source = `import {${name}} from './generators.mjs'; export default ctx => ${name}(ctx.services).build();`;
  fixtures.push({name:`HOST-SDK-${name}-frozen-v1`, input:native(source, examples, '0000000000000001', {}), expected});
}
// One process, with a fresh authoring runtime for every request as before.
const result = spawnSync(binary, ['--batch'], {input:JSON.stringify(fixtures.map(f => f.input)),
  encoding:'utf8',maxBuffer:16*1024*1024});
assert.equal(result.status, 0, result.stderr);
const actual = JSON.parse(result.stdout);
assert.equal(actual.length, fixtures.length);
fixtures.forEach((fixture, i) => {
  assert.equal(actual[i], contracts.canonicalJson(fixture.expected), fixture.name);
  console.log(`PASS ${fixture.name}`);
});
