// Demo fixtures: authoring modules evaluated by the procedural runtime, plus one
// hand-made contract asset that exercises products the kernel does not emit
// (nested assemblies, attachments, LOD and collision fallbacks).
import type {AssetViewOptions} from '../adapter.mjs';

export type JsonValue = null | boolean | number | string | JsonValue[] | {[key: string]: JsonValue};
export type JsonObject = {[key: string]: JsonValue};

/** Authoring SDK parameter spec (`sdk.parameters`) plus UI hints. The same
 *  object is embedded in the module source, so the UI and the recipe agree on
 *  bounds and the SDK still validates every value it is handed. */
export interface ParameterSpec {
  readonly type: 'number' | 'integer' | 'boolean';
  readonly default: number | boolean;
  readonly minimum?: number;
  readonly maximum?: number;
  readonly label: string;
  readonly step?: number;
}

export interface FixtureLoadContext { readonly signal: AbortSignal; readonly baseURL: URL }

interface FixtureBase {
  /** Stable fixture ID used in evidence. */
  readonly id: string;
  readonly title: string;
  readonly description: string;
  readonly parameters: Readonly<Record<string, ParameterSpec>>;
  /** Adapter options this fixture forces (merged over the UI's). */
  readonly viewOptions?: Partial<AssetViewOptions>;
  /** Present on error demonstrations: the code the fixture must fail with. */
  readonly expectError?: {readonly stage: 'runtime' | 'adapter'; readonly code: string};
}
export interface RecipeFixture extends FixtureBase {
  readonly kind: 'recipe';
  /** Self-contained authoring module (`export default (context) => ...`). */
  readonly source: string;
}
export interface AssetFixture extends FixtureBase {
  readonly kind: 'asset';
  /** Produces a contract asset document without the runtime. */
  load(context: FixtureLoadContext): Promise<{asset: unknown}>;
}
export type DemoFixture = RecipeFixture | AssetFixture;

const SDK_SPEC = (spec: Readonly<Record<string, ParameterSpec>>) =>
  JSON.stringify(Object.fromEntries(Object.entries(spec).map(([key, {type, default: value, minimum, maximum}]) =>
    [key, {type, default: value, ...(minimum === undefined ? {} : {minimum}), ...(maximum === undefined ? {} : {maximum})}])));

const builder = (id: string, extra = '') =>
  `new sdk.RecipeBuilder({source: {id: '${id}', uri: 'fixture://${id}', revision: job.source_revision}, seed, parameters: p, services${extra}})`;

const PROP_PARAMETERS = {
  sides: {type: 'integer', default: 6, minimum: 3, maximum: 24, label: 'Sides'},
  radius: {type: 'number', default: 0.8, minimum: 0.2, maximum: 2, step: 0.05, label: 'Radius (m)'},
  depth: {type: 'number', default: 1.2, minimum: 0.1, maximum: 3, step: 0.05, label: 'Height (m)'},
  checks: {type: 'integer', default: 8, minimum: 2, maximum: 64, label: 'Texture checks'},
  roughness: {type: 'number', default: 0.6, minimum: 0, maximum: 1, step: 0.05, label: 'Roughness'},
} as const satisfies Record<string, ParameterSpec>;

/** UV-mapped n-gon prism with a seeded checker texture, standing on an extruded
 *  plinth. The kernel's extrusion fills UV set 0 with zeros, so the textured
 *  prism is authored as `mesh.triangles` with explicit normals and UVs. That
 *  operation is optional, so the builder is offered exactly the host's
 *  capabilities and fails with unsupported_capability where it is missing. The
 *  texture bytes are made in the worker, addressed by their SHA-256 and handed
 *  to `evaluate`. */
const PROP_SOURCE = `const SPEC = ${SDK_SPEC(PROP_PARAMETERS)};
const hex = bytes => Array.from(bytes, x => x.toString(16).padStart(2, '0')).join('');
const PLINTH = 0.15;
export default async ({sdk, job, parameters, seed, services, capabilities, evaluate}) => {
  const p = sdk.parameters(SPEC, parameters);
  const b = ${builder('web.view.prop', ', supportedCapabilities: capabilities')};
  const rng = b.rng();
  const tint = () => [64 + rng.int(192), 64 + rng.int(192), 64 + rng.int(192), 255];
  const odd = tint(), even = tint(), n = p.checks, texels = new Uint8Array(n * n * 4);
  for (let row = 0; row < n; ++row)
    for (let column = 0; column < n; ++column) texels.set((row + column) % 2 ? odd : even, (row * n + column) * 4);
  const digest = hex(new Uint8Array(await crypto.subtle.digest('SHA-256', texels)));
  b.resource({id: 'checker.texels', content: {algorithm: 'sha256', hex: digest}, scalar_type: 'u8', components: 4,
    element_count: BigInt(n * n), byte_length: BigInt(texels.length)});
  b.texture({id: 'checker', resource: 'checker.texels', color_space: 'srgb', wrap_u: 'repeat', wrap_v: 'repeat',
    min_filter: 'nearest', mag_filter: 'nearest', width: n, height: n, pixel_format: 'rgba8'});
  const paint = b.material('painted', {baseColor: [1, 1, 1, 1], baseColorTexture: 'checker', uvSet: 0, roughness: p.roughness});
  const stone = b.material('stone', {baseColor: [0.35, 0.33, 0.3, 1], roughness: 0.9});
  // Prism from y = 0 to the height, vertex i at angle 2*pi*i/sides in the XZ
  // plane. Faces are CCW from outside with flat normals. Each side face spans
  // u 0..1 and v 0..height/width, so the checks stay square; caps are planar.
  const sides = p.sides, r = p.radius, h = p.depth, positions = [], normals = [], uv0 = [];
  const clean = x => x === 0 ? 0 : x;  // no negative zero in the canonical document
  const at = (t, y) => [clean(r * Math.cos(t)), y, clean(-r * Math.sin(t))];
  const cap = ([x, , z]) => [0.5 + x / (2 * r), 0.5 - z / (2 * r)];
  const tri = (p0, p1, p2, t0, t1, t2) => {
    const u = p1.map((x, k) => x - p0[k]), v = p2.map((x, k) => x - p0[k]);
    const c = [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]], length = Math.hypot(...c);
    positions.push(p0, p1, p2); uv0.push(t0, t1, t2);
    for (let k = 0; k < 3; ++k) normals.push(c.map(x => clean(x / length)));
  };
  const v = h / (2 * r * Math.sin(Math.PI / sides));
  for (let i = 0; i < sides; ++i) {
    const ta = 2 * Math.PI * i / sides, tb = 2 * Math.PI * (i + 1) / sides;
    const a0 = at(ta, 0), b0 = at(tb, 0), b1 = at(tb, h), a1 = at(ta, h);
    tri(a0, b0, b1, [0, 0], [1, 0], [1, v]);
    tri(a0, b1, a1, [0, 0], [1, v], [0, v]);
    tri([0, h, 0], a1, b1, [0.5, 0.5], cap(a1), cap(b1));
    tri([0, 0, 0], b0, a0, [0.5, 0.5], cap(b0), cap(a0));
  }
  const prism = b.node({id: 'prism', operation: 'mesh.triangles', parameters: {positions, normals, uv0, mode: 0}, materials: [paint]});
  const profile = b.polygon('plinth.profile', Array.from({length: sides}, (_, i) => {
    const t = 2 * Math.PI * i / sides; return [1.2 * r * Math.cos(t), 1.2 * r * Math.sin(t)];
  }));
  const plinth = b.extrude('plinth', profile, PLINTH, {materials: [stone]});
  // Extrusion runs along +Z; stand it up so +Z maps to +Y, below the prism.
  b.output(prism, b.place('plinth.upright', plinth, sdk.multiply(sdk.translation(0, -PLINTH, 0), sdk.rotation('x', -Math.PI / 2))));
  return evaluate(b.build(), [{id: 'checker.texels', bytes_hex: hex(texels)}]);
};
`;

const CSG_PARAMETERS = {
  size: {type: 'number', default: 1.6, minimum: 0.5, maximum: 3, step: 0.05, label: 'Block size (m)'},
  radius: {type: 'number', default: 0.95, minimum: 0.2, maximum: 2, step: 0.05, label: 'Cutter radius (m)'},
  segments: {type: 'integer', default: 24, minimum: 6, maximum: 48, label: 'Cutter segments'},
  jitter: {type: 'number', default: 0.2, minimum: 0, maximum: 0.5, step: 0.01, label: 'Seeded jitter'},
} as const satisfies Record<string, ParameterSpec>;

/** Box minus a seeded-offset sphere. The CSG node carries the material: v1 CSG
 *  output does not inherit operand materials and has no UV set. */
const CSG_SOURCE = `const SPEC = ${SDK_SPEC(CSG_PARAMETERS)};
export default ({sdk, job, parameters, seed, services}) => {
  const p = sdk.parameters(SPEC, parameters);
  const b = ${builder('web.view.csg')};
  const rng = b.rng();
  const clay = b.material('clay', {baseColor: [0.75, 0.25, 0.125, 1], roughness: 0.8});
  const h = p.size / 2, offset = () => h * 0.6 + rng.range(-p.jitter, p.jitter) * p.size;
  const block = b.box('block', [p.size, p.size, p.size]);
  const cutter = b.place('cutter', b.sphere('ball', p.radius, p.segments), sdk.translation(offset(), offset(), offset()));
  return b.output(b.csg('carved', 'subtract', [block, cutter], {materials: [clay]})).build();
};
`;

const ASSEMBLY_PARAMETERS = {
  count: {type: 'integer', default: 12, minimum: 1, maximum: 64, label: 'Bricks'},
  ring: {type: 'number', default: 2.2, minimum: 0.8, maximum: 6, step: 0.1, label: 'Ring radius (m)'},
  spheres: {type: 'integer', default: 4, minimum: 0, maximum: 16, label: 'Spheres'},
  mirror: {type: 'boolean', default: true, label: 'Mirror every 4th brick'},
  shear: {type: 'boolean', default: true, label: 'Shear every 4th brick'},
} as const satisfies Record<string, ParameterSpec>;

/** Two shared prototypes repeated with seeded rotations. Mirrored and sheared
 *  placements exercise the mirrored-instancing and individual-mesh paths. */
const ASSEMBLY_SOURCE = `const SPEC = ${SDK_SPEC(ASSEMBLY_PARAMETERS)};
export default ({sdk, job, parameters, seed, services}) => {
  const p = sdk.parameters(SPEC, parameters);
  const b = ${builder('web.view.assembly')};
  const rng = b.rng();
  const clay = b.material('clay', {baseColor: [0.75, 0.25, 0.125, 1], roughness: 0.8});
  const steel = b.material('steel', {baseColor: [0.62, 0.64, 0.68, 1], metallic: 1, roughness: 0.3});
  const brick = b.box('brick', [0.9, 0.45, 0.45], {materials: [clay]});
  const ball = b.sphere('ball', 0.3, 16, {materials: [steel]});
  const axes = ['x', 'y', 'z'], shear = [1, 0.6, 0, 0, 0, 1, 0, 0, 0.4, 0, 1, 0, 0, 0, 0, 1];
  const bricks = b.repeat('brick', p.count, brick, i => {
    const t = 2 * Math.PI * i / p.count;
    let m = sdk.multiply(sdk.translation(p.ring * Math.cos(t), 0.7 + 0.35 * Math.sin(3 * t), p.ring * Math.sin(t)),
      sdk.rotation(axes[i % 3], rng.range(0, 2 * Math.PI)));
    if (p.mirror && i % 4 === 1) m = sdk.multiply(m, sdk.scale(-1, 1, 1));
    if (p.shear && i % 4 === 3) m = sdk.multiply(m, shear);
    return m;
  });
  const balls = p.spheres === 0 ? [] : b.repeat('ball', p.spheres, ball, i => {
    const t = 2 * Math.PI * (i + rng.random()) / p.spheres;
    return sdk.translation(0.45 * p.ring * Math.cos(t), 0.3, 0.45 * p.ring * Math.sin(t));
  });
  return b.output(...bricks, ...balls).build();
};
`;

const translation = (x: number, y: number, z: number) => [1, 0, 0, x, 0, 1, 0, y, 0, 0, 1, z, 0, 0, 0, 1];

/** Hand-made contract example (nested `pair` assembly, mirrored non-uniform
 *  placement) decorated with products the web adapter only labels. */
async function loadContractFixture({signal, baseURL}: FixtureLoadContext): Promise<{asset: unknown}> {
  const url = new URL('libs/MatterContracts/examples/repeated-assembly.asset.json', baseURL);
  const response = await fetch(url, {signal});
  if (!response.ok) throw new Error(`fetch ${url.pathname} failed with HTTP ${response.status}`);
  const asset = await response.json() as Record<string, unknown> & {required_capabilities: {name: string; version: number}[]};
  asset.required_capabilities = [...asset.required_capabilities,
    {name: 'matter.asset.attachments', version: 1}, {name: 'matter.asset.collision', version: 1}, {name: 'matter.asset.lod', version: 1}];
  asset.attachments = [{id: 'socket.top', instance: 'placed.b', local_transform: translation(0, 1, 0)}];
  asset.lods = [{prototype: 'triangle', max_error_metres: 0.01}];
  asset.collisions = [{id: 'hull', instance: 'placed.a', shape: 'box', prototype: null, size: [1, 1, 0.1]}];
  asset.extensions = [{capability: {name: 'demo.sparkle', version: 1}, required: false, payload: {}}];
  return {asset};
}

export const FIXTURES: readonly DemoFixture[] = Object.freeze([
  {kind: 'recipe', id: 'WEB-VIEW-PROP', title: 'Prop: textured prism',
    description: 'UV-mapped n-gon prism (mesh.triangles) with a seeded checker texture (sRGB, nearest, UV set 0) made in the worker, on an extruded plinth.',
    parameters: PROP_PARAMETERS, source: PROP_SOURCE},
  {kind: 'recipe', id: 'WEB-VIEW-CSG', title: 'CSG: carved block',
    description: 'Box minus a sphere at a seeded offset; the CSG node carries the material.',
    parameters: CSG_PARAMETERS, source: CSG_SOURCE},
  {kind: 'recipe', id: 'WEB-VIEW-ASSEMBLY', title: 'Assembly: shared prototypes',
    description: 'Two shared prototypes, seeded rotations; mirrored placements batch with clockwise front faces, sheared ones draw individually.',
    parameters: ASSEMBLY_PARAMETERS, source: ASSEMBLY_SOURCE},
  {kind: 'asset', id: 'WEB-VIEW-CONTRACT', title: 'Contract: nested assembly + fallbacks',
    description: 'Hand-made contract asset: nested assembly, mirrored non-uniform placement, attachment anchor, and LOD/collision products the web view only labels.',
    parameters: {}, load: loadContractFixture},
  {kind: 'recipe', id: 'WEB-VIEW-ERR-SYNTAX', title: 'Error: module syntax',
    description: 'Authoring module that does not parse; the worker reports the failed module load as backend_failure.', parameters: {},
    source: 'export default ({sdk}) => { return sdk.RecipeBuilder( ;\n', expectError: {stage: 'runtime', code: 'backend_failure'}},
  {kind: 'recipe', id: 'WEB-VIEW-ERR-MIRROR-CSG', title: 'Error: mirrored CSG operand',
    description: 'CSG v1 accepts only rotation, translation and positive uniform scale.', parameters: {},
    source: `export default ({sdk, job, seed, services}) => {
  const p = {}; const b = ${builder('web.view.err.mirror')};
  const cutter = b.place('mirrored', b.sphere('ball', 0.8, 16), sdk.scale(-1, 1, 1));
  return b.output(b.csg('carved', 'subtract', [b.box('block', [1.5, 1.5, 1.5]), cutter])).build();
};
`, expectError: {stage: 'runtime', code: 'unsupported_capability'}},
  {kind: 'recipe', id: 'WEB-VIEW-ERR-REQUIRED-EXT', title: 'Error: required extension',
    description: 'Recipe requiring an extension no consumer implements.', parameters: {},
    source: `export default ({sdk, job, seed, services}) => {
  const p = {}; const b = ${builder('web.view.err.extension')};
  b.extension({capability: {name: 'demo.sparkle', version: 1}, required: true, payload: {glitter: 1}});
  return b.output(b.box('block', [1, 1, 1])).build();
};
`, expectError: {stage: 'runtime', code: 'unsupported_capability'}},
  {kind: 'recipe', id: 'WEB-VIEW-ERR-TEXTURE-LIMIT', title: 'Error: texture over the web limit',
    description: 'Valid prop whose 64x64 texture exceeds a 32-texel adapter limit (the kernel accepts it; the web view refuses).',
    parameters: {...PROP_PARAMETERS, checks: {...PROP_PARAMETERS.checks, default: 64}}, source: PROP_SOURCE,
    viewOptions: {maxTextureSize: 32}, expectError: {stage: 'adapter', code: 'resource_limit'}},
] satisfies DemoFixture[]);

export function fixture(id: string): DemoFixture {
  const found = FIXTURES.find(f => f.id === id);
  if (!found) throw new Error(`unknown fixture ${id}`);
  return found;
}

export function defaults(f: DemoFixture): JsonObject {
  return Object.fromEntries(Object.entries(f.parameters).map(([key, spec]) => [key, spec.default]));
}
