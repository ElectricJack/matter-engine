// Authoring generators for the native/browser parity harness. This exact text
// runs in the native portable host (QuickJS), in Node and in Chromium, so it
// imports nothing: each generator receives the SDK module and an authoring
// context ({parameters, seed, services, supportedCapabilities}) and returns a
// recipe document. run_parity.py requires byte-identical canonical JSON from
// every authoring host; the corpus generators must also reproduce the frozen
// fixtures/*.recipe.canonical.json bytes.
const CLAY = {baseColor: [0.75, 0.25, 0.125, 1], roughness: 0.8};
const METAL = {baseColor: [0.5, 0.5, 0.5, 1], metallic: 1, roughness: 0.25};
const source = name => ({id: `corpus.${name}`, uri: `fixture://component-separation/${name}`, revision: '1', content: null, legacy_hashes: []});

function builder(sdk, context, name) {
  const b = new sdk.RecipeBuilder({...context, source: source(name)});
  b.material('clay', CLAY);
  return b;
}
const clay = {materials: ['clay']};

// Frozen corpus: fixtures/primitive|extrude|ordered-csg|reversed-csg.
export function primitive(sdk, context) {
  const b = builder(sdk, context, 'primitive');
  return b.output(b.box('body', [2, 2, 2], clay)).build();
}
export function extrude(sdk, context) {
  const b = builder(sdk, context, 'extrude');
  const profile = b.polygon('profile', [[0, 0], [1, 0], [1, 1], [0, 1]], clay);
  return b.output(b.extrude('body', profile, 2, clay)).build();
}
function csg(sdk, context, operands) {
  // reversed-csg.recipe.json keeps the ordered-csg source identity; only the
  // operand order differs, which is the point of the case.
  const b = builder(sdk, context, 'ordered-csg');
  const cut = b.csg('cut', 'subtract', operands.map(id => sdk.nodeInput(id)), clay);
  b.box('outer', [2, 2, 2], clay);
  b.box('cutter', [1, 1, 1], clay);
  return b.output(cut).build();
}
export const orderedCsg = (sdk, context) => csg(sdk, context, ['outer', 'cutter']);
export const reversedCsg = (sdk, context) => csg(sdk, context, ['cutter', 'outer']);

// Harness cases without a frozen recipe. Their canonical bytes are pinned by
// hash in baseline.json and their geometry by analytic oracles in cases.json.
export function materialAssignment(sdk, context) {
  const b = builder(sdk, context, 'material-assignment');
  b.material('metal', METAL);
  const plain = b.box('clay_part', [1, 1, 1], clay);
  const metal = b.box('metal_part', [1, 1, 1], {materials: ['metal']});
  return b.output(plain, b.place('metal_placed', metal, sdk.translation(2, 0, 0))).build();
}
// The affine fixture's matrix (fixtures/affine.asset.json): mirrored in X,
// non-uniform scale, translated. Applied to a unit box so the result is a
// closed solid whose winding must be reversed to stay outward.
export const MIRROR = [-2, 0, 0, 10, 0, 3, 0, 20, 0, 0, 4, 30, 0, 0, 0, 1];
export function mirroredPlacement(sdk, context) {
  const b = builder(sdk, context, 'mirrored-placement');
  return b.output(b.place('mirrored', b.box('unit', [1, 1, 1], clay), MIRROR)).build();
}
export function sharedInstances(sdk, context) {
  const b = builder(sdk, context, 'shared-instances');
  const post = b.box('post', [0.25, 1, 0.25], clay);
  return b.output(...b.repeat('row', 4, post, i => sdk.translation(2 * i, 0, 0))).build();
}
// Box [1, 1 + first unit, 1] (fixtures/seeded-variation.json) plus the first
// eight words/units, compared exactly with fixtures/rng-splitmix64.json.
export function seededVariation(sdk, context) {
  const b = builder(sdk, context, 'seeded-variation');
  const words = [], units = [], wordRng = b.rng(), unitRng = b.rng();
  for (let i = 0; i < 8; ++i) {
    words.push(wordRng.nextU64().toString(16).padStart(16, '0'));
    units.push(unitRng.random());
  }
  b.output(b.box('body', [1, 1 + units[0], 1], clay));
  return b.metadata({words, units}).build();
}
// A pinned child dependency: the geometry service does not resolve assembly
// dependencies, so evaluation must fail explicitly rather than substitute.
export function repeatedAssembly(sdk, context) {
  const b = builder(sdk, context, 'repeated-assembly');
  const dep = b.dependency({id: 'shared.pair', content: {algorithm: 'sha256', hex: 'ab'.repeat(32)}, schema_version: {major: 1, minor: 0}});
  const pair = b.assembly('pair', dep);
  return b.output(...b.repeat('placed', 2, pair, i => i === 0 ? sdk.identity() : sdk.multiply(sdk.translation(3, 2, 1), sdk.scale(-2, 3, 4)))).build();
}
// Requires a capability no host supports; authoring must refuse explicitly.
export function unknownCapability(sdk, context) {
  const b = builder(sdk, context, 'unknown-capability');
  b.require({name: 'matter.operation.parity.unknown', version: 1});
  return b.output(b.box('body', [1, 1, 1], clay)).build();
}
