import { RecipeBuilder, nodeInput, parameters, identity, multiply, scale, translation } from '../index.mjs';

export function orderedCsg(services) {
  const builder = new RecipeBuilder({ source: { id: 'fixture.ordered-csg', uri: 'fixture://ordered-csg', revision: '1' },
    seed: '0123456789abcdef', parameters: { width: 0.1, enabled: true }, services });
  builder.material('clay', { baseColor: [0.75, 0.25, 0.125, 1], roughness: 0.8 });
  const cut = builder.at({ uri: 'fixture://ordered-csg', line: 12, column: 3 },
    () => builder.csg('cut', 'subtract', [nodeInput('outer'), nodeInput('cutter')], { materials: ['clay'] }));
  builder.box('outer', [2, 2, 2], { materials: ['clay'] });
  builder.sphere('cutter', 1, 16);
  return builder.output(cut);
}
export function extrudedProp(services) {
  const p = parameters({ depth: { type: 'number', default: 2, minimum: 0.01 }, enabled: { type: 'boolean', default: true } });
  const builder = new RecipeBuilder({ source: { id: 'example.prop', uri: 'example://prop', revision: '1' }, seed: 1n, parameters: p, services });
  const clay = builder.material('clay', { roughness: 0.8 });
  const profile = builder.polygon('profile', [[0, 0], [1, 0], [1, 1], [0, 1]]);
  return builder.output(builder.extrude('prop', profile, p.depth, { materials: [clay] }));
}
export function repeatedAssembly(services) {
  const builder = new RecipeBuilder({ source: { id: 'example.repeated', uri: 'example://repeated', revision: '1' }, seed: 0n, services });
  const dep = builder.dependency({ id: 'shared.pair', content: { algorithm: 'sha256', hex: 'ab'.repeat(32) }, schema_version: { major: 1, minor: 0 } });
  const reference = builder.assembly('pair', dep);
  const instances = builder.repeat('placed', 2, reference, i => i === 0 ? identity() : multiply(translation(3, 2, 1), scale(-2, 3, 4)));
  return builder.output(...instances);
}
