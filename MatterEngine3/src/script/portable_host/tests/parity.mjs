import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {spawnSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import * as sdk from '../../../../../packages/matter-authoring/index.mjs';
import * as contracts from '../../../../../libs/MatterContracts/examples/contracts.mjs';
import * as generators from './generators.mjs';
import {extrudedProp, orderedCsg, repeatedAssembly} from '../../../../../packages/matter-authoring/examples/recipes.mjs';
import {expectedProp, expectedRepeated} from '../../../../../packages/matter-authoring/tests/expected.mjs';
const binary = process.argv[2];
assert(binary, 'native test executable is required');
const services = sdk.contractServices(contracts, async bytes => new Uint8Array(createHash('sha256').update(bytes).digest()));
const fixtureSource = readFileSync(new URL('./generators.mjs', import.meta.url), 'utf8');
function native(source, helper, seed, parameters, legacy = false) {
  const input = {entry:'fixture/main.mjs', seed, parameters, modules:[
    {id:'fixture/main.mjs',uri:'fixture://native',dependencies:['fixture/generators.mjs'],source},
    {id:'fixture/generators.mjs',uri:'fixture://generators',dependencies:[],source:helper}]};
  if (legacy) input.legacy_voxels = true;
  const result = spawnSync(binary, ['--stdin'], {input:JSON.stringify(input),encoding:'utf8',maxBuffer:16*1024*1024});
  assert.equal(result.status, 0, result.stderr);
  return result.stdout;
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
    const result = native(source, fixtureSource, seed, parameters, name==='legacyCsg');
    assert.equal(result, contracts.canonicalJson(expected));
    if (name === 'seeded') {
      const vectors = JSON.parse(readFileSync(new URL('../../../../../libs/MatterContracts/tests/rng-vectors.json', import.meta.url)));
      const vector = vectors.vectors.find(v => v.seed === seed);
      assert.deepEqual(JSON.parse(result).metadata, {words:vector.words,units:vector.units});
    }
    console.log(`PASS HOST-SDK-${name}-${seed}`);
  }
}
// Independently frozen SDK/core fixtures, not only two runs of one generator.
const examples = readFileSync(new URL('../../../../../packages/matter-authoring/examples/recipes.mjs', import.meta.url), 'utf8').replace("'../index.mjs'", "'matter:authoring'");
const csg = JSON.parse(readFileSync(new URL('../../../../../libs/MatterContracts/examples/ordered-csg.recipe.json', import.meta.url)));
for (const [name, expected] of [['extrudedProp',expectedProp],['orderedCsg',csg],['repeatedAssembly',expectedRepeated]]) {
  const source = `import {${name}} from './generators.mjs'; export default ctx => ${name}(ctx.services).build();`;
  assert.equal(native(source, examples, '0000000000000001', {}), contracts.canonicalJson(expected));
  console.log(`PASS HOST-SDK-${name}-frozen-v1`);
}
