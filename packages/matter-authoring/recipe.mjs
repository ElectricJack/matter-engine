import { AuthoringError, PROFILE, RNG_ALGORITHM, DEFAULT_CAPABILITIES, affine, capability,
  checkCapability, copyJson, digest, fail, f32, fields, finite, freeze, hex64, id, identity,
  integer, location, need, seed64, unicode, vector, version } from './values.mjs';
import { SeededRng } from './rng.mjs';

const CORE = ['primitive.box', 'primitive.sphere', 'profile.polygon', 'profile.extrude',
  'solid.csg', 'mesh.input', 'placement.affine', 'assembly.reference'];
const DOCUMENT_FIELDS = ['document_type', 'canonical_profile', 'schema_version', 'required_capabilities',
  'resources', 'materials', 'textures', 'extensions', 'metadata', 'source', 'parameters', 'seed',
  'rng_algorithm', 'asset_dependencies', 'nodes', 'outputs', 'source_map'];
const NODE_FIELDS = ['id', 'operation', 'operation_version', 'output_type', 'inputs', 'parameters', 'transform', 'materials'];
const DEFAULT_LIMITS = Object.freeze({ maxNodes: 10000, maxDependencies: 1024, maxDepth: 128, maxValues: 1000000,
  maxStringBytes: 65536, maxResourceBytes: 64 * 1024 * 1024, maxVertices: 1000000, maxIndices: 3000000 });
function limitsOf(overrides = {}) {
  fields(overrides, Object.keys(DEFAULT_LIMITS), []);
  const limits = { ...DEFAULT_LIMITS, ...overrides };
  for (const [key, value] of Object.entries(limits)) integer(value, 1, DEFAULT_LIMITS[key], key);
  return limits;
}
function withContext(context, fn) {
  try { return fn(); } catch (error) {
    if (error instanceof AuthoringError && (!error.diagnostic.source || (!error.diagnostic.node_id && context.node_id)))
      throw new AuthoringError(error.diagnostic.code, error.diagnostic.message,
        { ...context, source: error.diagnostic.source ?? context.source, node_id: error.diagnostic.node_id || context.node_id,
          dependency_path: error.diagnostic.dependency_path.length ? error.diagnostic.dependency_path : context.dependency_path });
    throw error;
  }
}
function sourceIdentity(source) {
  fields(source, ['id', 'uri', 'revision', 'content', 'legacy_hashes'], ['id', 'uri', 'revision']);
  const result = { id: id(source.id), uri: unicode(source.uri), revision: unicode(source.revision),
    content: source.content == null ? null : digest(source.content), legacy_hashes: source.legacy_hashes ?? [] };
  need(Array.isArray(result.legacy_hashes), 'legacy hashes must be an array');
  const namespaces = new Set();
  for (const hash of result.legacy_hashes) {
    fields(hash, ['namespace', 'value']);
    need(['matter.source.v1', 'matter.resolved.v1'].includes(hash.namespace), 'legacy hashes require an explicit Matter namespace');
    need(!namespaces.has(hash.namespace), 'duplicate legacy hash namespace'); namespaces.add(hash.namespace);
    need(typeof hash.value === 'string' && /^[0-9a-f]{16}$/.test(hash.value), 'legacy hash must be fixed uint64 hex');
  }
  return result;
}
function dependency(value) {
  fields(value, ['id', 'content', 'schema_version']);
  return { id: id(value.id), content: digest(value.content), schema_version: version(value.schema_version) };
}
function unique(values, label) {
  need(Array.isArray(values), `${label} must be an array`);
  const map = new Map();
  for (const value of values) { fields(value); id(value.id); need(!map.has(value.id), `duplicate ${label} ID ${value.id}`); map.set(value.id, value); }
  return map;
}
export function nodeInput(reference, type = 'mesh', port = 'input') {
  need(['mesh', 'profile', 'assembly'].includes(type), 'invalid node input type');
  return freeze({ port: id(port), kind: 'node', reference: id(reference), type });
}
function inputOf(reference, port) {
  if (typeof reference === 'string') return nodeInput(reference, 'mesh', port);
  fields(reference, ['port', 'kind', 'reference', 'type']); need(reference.kind === 'node', 'node reference required');
  return nodeInput(reference.reference, reference.type, port);
}
function checkMaterial(material, textures) {
  fields(material, ['id', 'base_color', 'metallic', 'roughness', 'normal_scale', 'emissive',
    'metallic_roughness_texture', 'normal_texture', 'emissive_texture']);
  id(material.id); fields(material.base_color, ['factor', 'texture', 'uv_set']);
  const color = vector(material.base_color.factor, 4, f32);
  need(color.every(x => x >= 0 && x <= 1) && color[3] === 1, 'baseline PBR base color must be opaque linear RGBA in [0,1]');
  integer(material.base_color.uv_set);
  for (const key of ['metallic', 'roughness']) need(finite(material[key]) >= 0 && material[key] <= 1, `${key} must be in [0,1]`);
  need(finite(material.normal_scale) >= 0, 'normal scale must be nonnegative');
  need(vector(material.emissive, 3, f32).every(x => x >= 0), 'emissive must be nonnegative');
  for (const texture of [material.base_color.texture, material.metallic_roughness_texture, material.normal_texture, material.emissive_texture])
    if (texture !== null) { id(texture); if (!textures.has(texture)) fail('missing_dependency', `material ${material.id} needs texture ${texture}`); }
  for (const key of ['metallic_roughness_texture', 'normal_texture']) if (material[key] !== null)
    need(textures.get(material[key]).color_space === 'linear', 'data textures require linear color space');
  material.base_color.factor = color;
  for (const key of ['metallic', 'roughness', 'normal_scale']) material[key] = f32(material[key]);
  material.emissive = vector(material.emissive, 3, f32);
}
// Own, validate and normalize a wire document. Resolved buffer bytes are the
// evaluator's responsibility; this boundary validates their declarations.
export function validateRecipe(document, options = {}) {
  const limits = limitsOf(options.limits);
  const d = copyJson(document, limits), supported = copyJson(options.supportedCapabilities ?? DEFAULT_CAPABILITIES);
  need(Array.isArray(supported), 'supported capabilities must be an array');
  fields(d, DOCUMENT_FIELDS);
  need(d.document_type === 'recipe', 'recipe document required');
  if (d.canonical_profile !== PROFILE || d.rng_algorithm !== RNG_ALGORITHM)
    fail('unsupported_version', 'unsupported recipe canonical profile or RNG algorithm');
  version(d.schema_version); fields(d.source, ['id', 'uri', 'revision', 'content', 'legacy_hashes']); d.source = sourceIdentity(d.source);
  need(typeof d.seed === 'string' && /^[0-9a-f]{16}$/.test(d.seed), 'wire seed must be fixed uint64 hex');
  fields(d.parameters); fields(d.source_map);
  for (const key of ['nodes', 'resources', 'textures', 'materials', 'asset_dependencies', 'required_capabilities', 'outputs', 'extensions'])
    need(Array.isArray(d[key]), `${key} must be an array`);
  if (d.nodes.length > limits.maxNodes || d.asset_dependencies.length > limits.maxDependencies)
    fail('resource_limit', 'recipe node/dependency budget exceeded');
  supported.forEach(capability);
  const declared = new Set(), declaredNames = new Set();
  for (const cap of d.required_capabilities) {
    checkCapability(cap, supported); const key = `${cap.name}@${cap.version}`;
    need(!declaredNames.has(cap.name), 'duplicate required capability name'); declaredNames.add(cap.name); declared.add(key);
  }
  const requireDeclared = cap => { need(declared.has(`${cap.name}@${cap.version}`), `missing capability declaration ${cap.name}@${cap.version}`); checkCapability(cap, supported); };
  requireDeclared({ name: 'matter.recipe.core', version: 1 });
  const nodes = unique(d.nodes, 'node'), materials = unique(d.materials, 'material'), resources = unique(d.resources, 'resource'),
    assets = unique(d.asset_dependencies, 'dependency'), textures = unique(d.textures, 'texture');
  d.asset_dependencies.forEach(dependency);
  let resourceBytes = 0n;
  for (const resource of d.resources) {
    fields(resource, ['id', 'content', 'scalar_type', 'components', 'element_count', 'byte_length']); digest(resource.content);
    need(['u8', 'u32', 'f32'].includes(resource.scalar_type), 'invalid resource scalar type'); integer(resource.components, 1, 4);
    for (const key of ['element_count', 'byte_length']) need(typeof resource[key] === 'string' && /^[0-9a-f]{16}$/.test(resource[key]), 'resource counts must be uint64 hex');
    const expected = seed64(resource.element_count) * BigInt(resource.components) * (resource.scalar_type === 'u8' ? 1n : 4n);
    need(expected <= 0xffffffffffffffffn && expected === seed64(resource.byte_length), 'resource byte length/count mismatch');
    resourceBytes += expected;
    if (resourceBytes > BigInt(limits.maxResourceBytes)) fail('resource_limit', 'resource byte budget exceeded');
  }
  for (const texture of d.textures) {
    fields(texture, ['id', 'resource', 'color_space', 'wrap_u', 'wrap_v', 'min_filter', 'mag_filter', 'width', 'height', 'pixel_format']);
    id(texture.resource); integer(texture.width, 1); integer(texture.height, 1);
    need(['linear', 'srgb'].includes(texture.color_space) && ['repeat', 'clamp', 'mirror'].includes(texture.wrap_u) && ['repeat', 'clamp', 'mirror'].includes(texture.wrap_v), 'invalid texture color space/wrap');
    need(['nearest', 'linear'].includes(texture.min_filter) && ['nearest', 'linear'].includes(texture.mag_filter) && texture.pixel_format === 'rgba8', 'invalid texture format/filter');
    if (!resources.has(texture.resource)) fail('missing_dependency', `texture ${texture.id} needs resource ${texture.resource}`);
    const r = resources.get(texture.resource);
    need(r.scalar_type === 'u8' && r.components === 4 && seed64(r.element_count) === BigInt(texture.width) * BigInt(texture.height), 'texture resource must be tightly packed u8x4');
  }
  if (materials.size || textures.size) requireDeclared({ name: 'matter.material.pbr', version: 1 });
  d.materials.forEach(m => checkMaterial(m, textures));
  for (const [key, source] of Object.entries(d.source_map)) { if (!nodes.has(key)) fail('missing_dependency', `source map references missing node ${key}`); location(source); }
  const sourceFor = key => Object.hasOwn(d.source_map, key) ? d.source_map[key] : undefined;
  for (const n of d.nodes) withContext({ node_id: n.id, source: sourceFor(n.id) }, () => {
    fields(n, NODE_FIELDS); unicode(n.operation); integer(n.operation_version, 1);
    need(['mesh', 'profile', 'assembly'].includes(n.output_type), 'invalid node output type');
    need(Array.isArray(n.inputs) && Array.isArray(n.materials), 'node inputs/materials must be arrays');
    n.transform = affine(n.transform); fields(n.parameters);
    const builtin = CORE.includes(n.operation);
    if (builtin && n.operation_version !== 1) fail('unsupported_version', 'core operation version must be 1');
    if (!builtin) requireDeclared({ name: `matter.operation.${n.operation}`, version: n.operation_version });
    for (const material of n.materials) if (!materials.has(id(material))) fail('missing_dependency', `missing material ${material}`);
    need(new Set(n.materials).size === n.materials.length, 'duplicate node material');
    for (const input of n.inputs) {
      fields(input, ['port', 'kind', 'reference', 'type']); id(input.port); id(input.reference);
      need(['mesh', 'profile', 'assembly'].includes(input.type), 'invalid input type');
      const map = input.kind === 'node' ? nodes : input.kind === 'resource' ? resources : input.kind === 'asset' ? assets : null;
      need(map, 'invalid input kind');
      if (!map.has(input.reference)) fail('missing_dependency', `missing ${input.kind} ${input.reference}`, { node_id: n.id, dependency_path: [n.id, input.reference] });
      if (input.kind === 'node') need(map.get(input.reference).output_type === input.type, 'typed node input mismatch');
    }
    if (!builtin) return;
    const expectedType = n.operation === 'profile.polygon' ? 'profile' : n.operation === 'assembly.reference' ? 'assembly' : n.operation === 'placement.affine' ? n.output_type : 'mesh';
    need(n.output_type === expectedType, 'operation output type mismatch');
    const parameterKeys = { 'primitive.box': ['size'], 'primitive.sphere': ['radius', 'segments'], 'profile.polygon': ['points'],
      'profile.extrude': ['depth'], 'solid.csg': ['operator'], 'mesh.input': [], 'placement.affine': [], 'assembly.reference': [] };
    fields(n.parameters, parameterKeys[n.operation]);
    const count = n.operation.startsWith('primitive.') || n.operation === 'profile.polygon' ? 0 : n.operation === 'solid.csg' || n.operation === 'mesh.input' ? null : 1;
    need(count === null ? n.inputs.length >= 2 : n.inputs.length === count, 'operation input count mismatch');
    if (n.operation === 'primitive.box') need(vector(n.parameters.size, 3).every(x => x > 0), 'box size must be positive');
    if (n.operation === 'primitive.sphere') { need(finite(n.parameters.radius) > 0, 'sphere radius must be positive'); integer(n.parameters.segments, 3, 65536); }
    if (n.operation === 'profile.polygon') {
      need(Array.isArray(n.parameters.points) && n.parameters.points.length >= 3, 'polygon requires at least three points'); n.parameters.points.forEach(p => vector(p, 2));
    }
    if (n.operation === 'profile.extrude') need(finite(n.parameters.depth) > 0, 'extrude depth must be positive');
    if (n.operation === 'solid.csg') need(['union', 'subtract', 'intersect'].includes(n.parameters.operator), 'invalid ordered CSG operator');
    for (const input of n.inputs) {
      if (n.operation === 'mesh.input') need(input.kind === 'resource' && input.type === 'mesh', 'mesh input requires typed resource');
      else if (n.operation === 'assembly.reference') need(input.kind === 'asset' && input.type === 'assembly', 'assembly reference needs an asset dependency');
      else need(input.kind === 'node', 'operation requires node input');
      if (n.operation === 'profile.extrude') need(input.type === 'profile', 'extrude requires profile');
      if (n.operation === 'solid.csg') need(input.type === 'mesh', 'CSG requires mesh');
      if (n.operation === 'placement.affine') need(input.type === n.output_type && input.type !== 'profile', 'placement input/output type mismatch');
    }
    if (!['solid.csg', 'placement.affine'].includes(n.operation)) need(n.transform.every((v, i) => v === identity()[i]), 'only CSG/affine placement carry transforms');
    if (n.operation === 'mesh.input') {
      const ports = new Map();
      for (const input of n.inputs) { need(['positions', 'indices', 'normals', 'tangents', 'uv0'].includes(input.port) && !ports.has(input.port), 'unknown/duplicate mesh buffer port'); ports.set(input.port, resources.get(input.reference)); }
      need(ports.has('positions') && ports.has('indices') && n.materials.length === 1, 'mesh input requires positions, indices and one whole-mesh material');
      const vertices = seed64(ports.get('positions').element_count); need(vertices > 0n, 'mesh needs vertices');
      for (const [port, r] of ports) {
        const index = port === 'indices', components = index ? 1 : port === 'tangents' ? 4 : port === 'uv0' ? 2 : 3;
        need(r.scalar_type === (index ? 'u32' : 'f32') && r.components === components, 'mesh buffer type mismatch');
        const count = seed64(r.element_count);
        need(index ? count > 0n && count % 3n === 0n : count === vertices, 'mesh buffer element count mismatch');
        if (count > BigInt(index ? limits.maxIndices : limits.maxVertices)) fail('resource_limit', 'mesh index/vertex budget exceeded');
      }
    }
  });
  const done = new Map(), active = new Set();
  function visit(key, path) {
    if (active.has(key)) fail('cycle', `recipe graph cycle: ${[...path, key].join(' -> ')}`, { node_id: key, source: sourceFor(key), dependency_path: [...path, key] });
    if (path.length > limits.maxDepth) fail('resource_limit', 'recipe graph depth exceeded', { node_id: key, source: sourceFor(key), dependency_path: [...path, key] });
    if (done.has(key)) return done.get(key);
    active.add(key);
    let longest = [];
    for (const input of nodes.get(key).inputs) if (input.kind === 'node') {
      const childPath = visit(input.reference, [...path, key]);
      if (childPath.length > longest.length) longest = childPath;
    }
    const result = [key, ...longest];
    if (result.length - 1 > limits.maxDepth) fail('resource_limit', 'recipe graph depth exceeded',
      { node_id: key, source: sourceFor(key), dependency_path: result });
    active.delete(key); done.set(key, result); return result;
  }
  for (const key of nodes.keys()) visit(key, []);
  for (const n of d.nodes) if (n.operation === 'solid.csg') {
    const seen = new Set(), pending = [n.id];
    while (pending.length) {
      const key = pending.pop(); if (seen.has(key)) continue; seen.add(key);
      const child = nodes.get(key);
      affine(child.transform, true, { node_id: n.id, source: sourceFor(n.id), dependency_path: [n.id, key] });
      for (const input of child.inputs) if (input.kind === 'node') pending.push(input.reference);
    }
  }
  need(d.outputs.length > 0 && new Set(d.outputs).size === d.outputs.length, 'recipe requires unique outputs');
  for (const output of d.outputs) if (!nodes.has(id(output))) fail('missing_dependency', `missing output ${output}`);
  const extensionNames = new Set();
  for (const extension of d.extensions) {
    fields(extension, ['capability', 'required', 'payload']); capability(extension.capability); need(typeof extension.required === 'boolean', 'extension required flag must be boolean');
    need(!extensionNames.has(extension.capability.name), 'duplicate extension envelope'); extensionNames.add(extension.capability.name);
    if (extension.required) requireDeclared(extension.capability);
  }
  return freeze(d);
}

export class RecipeBuilder {
  #document; #services; #supported; #limits; #source = null;
  constructor({ source, seed, parameters = {}, requiredCapabilities = [{ name: 'matter.recipe.core', version: 1 }], supportedCapabilities = DEFAULT_CAPABILITIES, services, limits } = {}) {
    this.#limits = limitsOf(limits); this.#services = services; this.#supported = copyJson(supportedCapabilities);
    need(Array.isArray(this.#supported) && Array.isArray(requiredCapabilities), 'capabilities must be arrays');
    this.#supported.forEach(capability);
    this.#document = { document_type: 'recipe', canonical_profile: PROFILE, schema_version: { major: 1, minor: 0 },
      required_capabilities: [], resources: [], materials: [], textures: [], extensions: [], metadata: {},
      source: sourceIdentity(copyJson(source)), parameters: copyJson(parameters, this.#limits), seed: hex64(seed), rng_algorithm: RNG_ALGORITHM,
      asset_dependencies: [], nodes: [], outputs: [], source_map: {} };
    fields(this.#document.parameters);
    for (const cap of requiredCapabilities) this.require(cap);
  }
  require(cap) {
    cap = copyJson(cap); checkCapability(cap, this.#supported, { source: this.#source });
    if (!this.#document.required_capabilities.some(c => c.name === cap.name && c.version === cap.version)) this.#document.required_capabilities.push(cap);
    return this;
  }
  at(source, callback) {
    const prior = this.#source; this.#source = location(copyJson(source));
    try { return withContext({ source: this.#source }, () => callback(this)); } finally { this.#source = prior; }
  }
  node({ id: key, operation, operationVersion = 1, outputType = 'mesh', inputs = [], parameters = {}, materials = [], transform = identity(), source } = {}) {
    const context = { node_id: key, source: source ?? this.#source };
    return withContext(context, () => {
      id(key); unicode(operation); integer(operationVersion, 1);
      need(['mesh', 'profile', 'assembly'].includes(outputType), 'invalid node output type');
      if (this.#document.nodes.length >= this.#limits.maxNodes) fail('resource_limit', 'node budget exceeded');
      need(!this.#document.nodes.some(n => n.id === key), `duplicate node ${key}`);
      if (!CORE.includes(operation)) this.require({ name: `matter.operation.${operation}`, version: operationVersion });
      const n = copyJson({ id: key, operation, operation_version: operationVersion, output_type: outputType, inputs, parameters, materials, transform: affine(transform) }, this.#limits);
      if (context.source) this.#document.source_map[key] = location(copyJson(context.source));
      this.#document.nodes.push(n); return nodeInput(key, outputType);
    });
  }
  box(key, size, options = {}) { return this.node({ ...options, id: key, operation: 'primitive.box', parameters: { size } }); }
  sphere(key, radius, segments = 16, options = {}) { return this.node({ ...options, id: key, operation: 'primitive.sphere', parameters: { radius, segments } }); }
  polygon(key, points, options = {}) { return this.node({ ...options, id: key, operation: 'profile.polygon', outputType: 'profile', parameters: { points } }); }
  extrude(key, profile, depth, options = {}) { return this.node({ ...options, id: key, operation: 'profile.extrude', inputs: [inputOf(profile, 'profile')], parameters: { depth } }); }
  csg(key, operator, operands, options = {}) { need(Array.isArray(operands), 'CSG operands must be an ordered array'); return this.node({ ...options, id: key, operation: 'solid.csg', inputs: operands.map(ref => inputOf(ref, 'operand')), parameters: { operator } }); }
  place(key, reference, transform, options = {}) {
    const input = inputOf(reference, 'input');
    return this.node({ ...options, id: key, operation: 'placement.affine', outputType: input.type, inputs: [input], transform });
  }
  assembly(key, dependencyId, options = {}) { return this.node({ ...options, id: key, operation: 'assembly.reference', outputType: 'assembly', inputs: [{ port: 'assembly', kind: 'asset', reference: dependencyId, type: 'assembly' }] }); }
  mesh(key, buffers, material, options = {}) {
    buffers = copyJson(buffers); fields(buffers);
    return this.node({ ...options, id: key, operation: 'mesh.input', inputs: Object.entries(buffers).map(([port, reference]) => ({ port, kind: 'resource', reference, type: 'mesh' })), materials: [material] });
  }
  repeat(prefix, count, reference, transformAt) {
    integer(count, 0, this.#limits.maxNodes, 'repeat count'); id(prefix);
    if (this.#document.nodes.length + count > this.#limits.maxNodes) fail('resource_limit', 'repeat exceeds node budget');
    need(typeof transformAt === 'function', 'repeat requires an explicit transform function');
    const staged = copyJson(this.#document);
    try { return Object.freeze(Array.from({ length: count }, (_, i) => this.place(`${prefix}.${i}`, reference, transformAt(i)))); }
    catch (error) { this.#document = staged; throw error; }
  }
  dependency(value) {
    const next = dependency(copyJson(value)), old = this.#document.asset_dependencies.find(d => d.id === next.id);
    if (old) need(old.content.hex === next.content.hex && old.schema_version.major === next.schema_version.major && old.schema_version.minor === next.schema_version.minor, `conflicting pinned dependency ${next.id}`);
    else {
      if (this.#document.asset_dependencies.length >= this.#limits.maxDependencies) fail('resource_limit', 'dependency budget exceeded');
      this.#document.asset_dependencies.push(next);
    }
    return next.id;
  }
  resource(value) {
    fields(value, ['id', 'content', 'scalar_type', 'components', 'element_count', 'byte_length']);
    const normalized = {};
    need(Object.getOwnPropertySymbols(value).length === 0, 'symbol resource keys are forbidden');
    for (const key of Object.keys(value)) {
      const descriptor = Object.getOwnPropertyDescriptor(value, key);
      need(Object.hasOwn(descriptor, 'value'), 'resource accessors are forbidden');
      normalized[key] = ['element_count', 'byte_length'].includes(key) ? hex64(descriptor.value) : descriptor.value;
    }
    const next = copyJson(normalized); id(next.id);
    need(!this.#document.resources.some(r => r.id === next.id), 'duplicate resource ID'); this.#document.resources.push(next); return next.id;
  }
  texture(value) { value = copyJson(value); fields(value); id(value.id); need(!this.#document.textures.some(t => t.id === value.id), 'duplicate texture ID'); this.require({ name: 'matter.material.pbr', version: 1 }); this.#document.textures.push(value); return value.id; }
  material(key, options = {}) {
    options = copyJson(options);
    fields(options, ['baseColor', 'metallic', 'roughness', 'normalScale', 'emissive', 'baseColorTexture', 'uvSet', 'metallicRoughnessTexture', 'normalTexture', 'emissiveTexture'], []);
    this.require({ name: 'matter.material.pbr', version: 1 });
    need(!this.#document.materials.some(m => m.id === key), 'duplicate material ID');
    this.#document.materials.push({ id: id(key), base_color: { factor: vector(options.baseColor ?? [1, 1, 1, 1], 4, f32), texture: options.baseColorTexture ?? null, uv_set: options.uvSet ?? 0 },
      metallic: f32(options.metallic ?? 0), roughness: f32(options.roughness ?? 1), normal_scale: f32(options.normalScale ?? 1), emissive: vector(options.emissive ?? [0, 0, 0], 3, f32),
      metallic_roughness_texture: options.metallicRoughnessTexture ?? null, normal_texture: options.normalTexture ?? null, emissive_texture: options.emissiveTexture ?? null });
    return key;
  }
  extension(value) { value = copyJson(value, this.#limits); fields(value, ['capability', 'required', 'payload']); capability(value.capability); need(!this.#document.extensions.some(e => e.capability.name === value.capability.name), 'duplicate extension envelope'); if (value.required) this.require(value.capability); this.#document.extensions.push(value); return this; }
  metadata(value) { this.#document.metadata = copyJson(value, this.#limits); return this; }
  output(...references) { this.#document.outputs = references.map(ref => typeof ref === 'string' ? id(ref) : inputOf(ref, 'output').reference); return this; }
  compose(prefix, document) {
    id(prefix); const d = validateRecipe(document, { supportedCapabilities: this.#supported, limits: this.#limits });
    const rename = key => id(`${prefix}/${key}`);
    // Keep dependency IDs pinned and shared; all document-local IDs are scoped.
    const staged = copyJson(this.#document);
    try {
      for (const cap of d.required_capabilities) this.require(cap);
      for (const dep of d.asset_dependencies) this.dependency(dep);
      for (const r of d.resources) this.resource({ ...r, id: rename(r.id) });
      for (const t of d.textures) this.texture({ ...t, id: rename(t.id), resource: rename(t.resource) });
      for (const m of d.materials) {
        const remap = key => key === null ? null : rename(key);
        const next = { ...m, id: rename(m.id), base_color: { ...m.base_color, texture: remap(m.base_color.texture) },
          metallic_roughness_texture: remap(m.metallic_roughness_texture), normal_texture: remap(m.normal_texture), emissive_texture: remap(m.emissive_texture) };
        need(!this.#document.materials.some(old => old.id === next.id), 'composition material ID collision'); this.#document.materials.push(copyJson(next));
      }
      // Unknown extension payloads may contain IDs: never guess how to rewrite them.
      if (d.extensions.length) fail('unsupported_capability', 'compose extension-bearing recipes through an explicit extension adapter');
      for (const n of d.nodes) {
        if (!CORE.includes(n.operation)) fail('unsupported_capability', 'compose custom operations through an explicit operation adapter');
        this.node({ id: rename(n.id), operation: n.operation, operationVersion: n.operation_version, outputType: n.output_type,
          inputs: n.inputs.map(input => ({ ...input, reference: input.kind === 'asset' ? input.reference : rename(input.reference) })),
          parameters: n.parameters, transform: n.transform, materials: n.materials.map(rename), source: Object.hasOwn(d.source_map, n.id) ? d.source_map[n.id] : { uri: d.source.uri, line: 0, column: 0 } });
      }
      return Object.freeze(d.outputs.map(key => nodeInput(rename(key), d.nodes.find(n => n.id === key).output_type)));
    } catch (error) { this.#document = staged; throw error; }
  }
  rng() { return new SeededRng(this.#document.seed, this.#services); }
  build() { return validateRecipe(this.#document, { supportedCapabilities: this.#supported, limits: this.#limits }); }
  canonical() {
    if (this.#services?.canonicalProfile !== PROFILE || typeof this.#services.canonicalJson !== 'function') fail('unsupported_capability', `inject ${PROFILE} canonical services`);
    const document = this.build();
    try { return this.#services.canonicalJson(document); }
    catch (error) { if (error instanceof RangeError) fail('resource_limit', `contract canonical service: ${error.message}`); throw error; }
  }
  async digest() {
    if (typeof this.#services?.digestDocument !== 'function') fail('unsupported_capability', 'inject a SHA-256 document digest service');
    const document = this.build();
    try { return digest(await this.#services.digestDocument(document)); }
    catch (error) { if (error instanceof RangeError) fail('resource_limit', `contract digest service: ${error.message}`); throw error; }
  }
}
