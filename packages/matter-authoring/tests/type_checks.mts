import { RecipeBuilder, parameters, nodeInput, scale, translation, type ContractServices } from '../index.mjs';
declare const services: ContractServices;
const p = parameters({ depth: { type: 'number', default: 2 }, mode: { type: 'enum', default: 'solid', values: ['solid', 'hollow'] } });
const depth: number = p.depth;
const mode: 'solid' | 'hollow' = p.mode;
void depth; void mode;
const b = new RecipeBuilder({ source: { id: 'typed', uri: 'fixture://types', revision: '1' }, seed: 1n, services, parameters: p });
const profile = b.polygon('profile', [[0, 0], [1, 0], [0, 1]]);
const mesh = b.extrude('mesh', profile, p.depth);
b.place('mirrored', mesh, scale(-2, 3, 4));
b.csg('cut', 'subtract', [mesh, nodeInput('later')]);
b.box('later', [1, 1, 1]);
b.repeat('copies', 2, mesh, i => translation(i, 0, 0));
b.output(mesh).build();
// @ts-expect-error a Number seed is not a lossless uint64
new RecipeBuilder({ source: { id: 'bad', uri: '', revision: '' }, seed: 123 });
// @ts-expect-error profiles cannot be CSG operands
b.csg('wrong', 'subtract', [mesh, profile]);
// @ts-expect-error extrusion requires a profile
b.extrude('wrong', mesh, 1);
// @ts-expect-error parameter overrides preserve their declared type
parameters({ enabled: { type: 'boolean', default: true } }, { enabled: 'yes' });
// @ts-expect-error unknown override
parameters({ depth: { type: 'number', default: 1 } }, { other: 2 });
