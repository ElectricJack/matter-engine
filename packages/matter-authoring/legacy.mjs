import { AuthoringError, affine, copyJson, fail, f32, fields, freeze, id, identity, location, multiply,
  need, rotation, scale, translation, vector } from './values.mjs';

export const LEGACY_VOXELS = freeze({ name: 'matter.legacy.voxel-meshing', version: 1 });
// This adapter constructs recipes. It never installs globals or executes modules.
export class LegacyPart {
  #builder; #options; #stack = [identity()]; #sequence = 0; #material = null;
  #session = null; #smoothing = 0; #outputs = []; #finished = false; #sealed = false; #voxelSessions = []; #extensionEmitted = false;
  constructor(builder, options = {}) {
    this.#builder = builder; this.#options = options;
    id(options.prefix ?? 'legacy');
  }
  #context() {
    const supplied = this.#options.sourceLocation?.();
    return supplied ? { source: location(copyJson(supplied)) } : {};
  }
  #key() { return `${this.#options.prefix ?? 'legacy'}.${this.#sequence++}`; }
  #run(verb, fn) {
    const context = this.#context();
    try {
      need(!this.#finished, 'legacy authoring has already finished');
      need(!this.#sealed || verb === 'finish', 'finish has sealed legacy authoring; fix the builder diagnostic and retry finish');
      return context.source ? this.#builder.at(context.source, fn) : fn();
    } catch (error) {
      if (error instanceof AuthoringError && !error.diagnostic.source)
        throw new AuthoringError(error.diagnostic.code, `legacy ${verb}: ${error.diagnostic.message}`, { ...context, node_id: error.diagnostic.node_id, dependency_path: error.diagnostic.dependency_path });
      throw error;
    }
  }
  invoke(verb, ...args) {
    const supported = ['pushMatrix', 'popMatrix', 'translate', 'rotateX', 'rotateY', 'rotateZ', 'scale', 'applyMatrix',
      'fill', 'box', 'sphere', 'beginVoxels', 'endVoxels', 'smoothing', 'union', 'difference', 'intersection', 'placeChild', 'finish'];
    if (!supported.includes(verb)) fail('unsupported_capability', `legacy verb ${verb} is unavailable in the portable subset; retain the native path or supply a versioned capability adapter`, this.#context());
    return this[verb](...args);
  }
  pushMatrix() { return this.#run('pushMatrix', () => { this.#stack.push([...this.#stack.at(-1)]); }); }
  popMatrix() { return this.#run('popMatrix', () => { need(this.#stack.length > 1, 'popMatrix without matching pushMatrix'); this.#stack.pop(); }); }
  #transform(verb, local) { return this.#run(verb, () => { this.#stack[this.#stack.length - 1] = multiply(this.#stack.at(-1), local()); }); }
  translate(x, y, z) { return this.#transform('translate', () => translation(x, y, z)); }
  scale(x, y, z) { return this.#transform('scale', () => scale(x, y, z)); }
  rotateX(r) { return this.#transform('rotateX', () => rotation('x', r)); }
  rotateY(r) { return this.#transform('rotateY', () => rotation('y', r)); }
  rotateZ(r) { return this.#transform('rotateZ', () => rotation('z', r)); }
  applyMatrix(m) { return this.#transform('applyMatrix', () => affine(m)); }
  fill(material) {
    return this.#run('fill', () => {
      const mapped = typeof material === 'string' ? material : this.#options.materialIds?.[material];
      if (mapped === undefined) fail('missing_dependency', `map native material ${material} to an explicit PBR material ID`);
      this.#material = id(mapped);
    });
  }
  #brush(kind, center, shape) {
    return this.#run(kind, () => {
      center = vector(center, 3, f32);
      const transform = multiply(this.#stack.at(-1), translation(...center));
      if (this.#session) affine(transform, true);
      const options = { materials: this.#material === null ? [] : [this.#material] };
      const primitive = kind === 'box' ? this.#builder.box(this.#key(), vector(shape, 3, f32).map(v => f32(2 * v)), options)
        : this.#builder.sphere(this.#key(), f32(shape), 12, options);
      const placed = this.#builder.place(this.#key(), primitive, transform);
      if (this.#session) this.#session.brushes.push({ reference: placed, operator: 'union' });
      else this.#outputs.push(placed);
      return placed;
    });
  }
  box(center, halfExtents) { return this.#brush('box', center, halfExtents); }
  sphere(center, radius) { return this.#brush('sphere', center, radius); }
  beginVoxels(spacing = 0.1) {
    return this.#run('beginVoxels', () => {
      need(!this.#session, 'voxel sessions cannot nest');
      const step = f32(spacing); // Native nonpositive spacing falls back to 0.1m.
      this.#builder.require(LEGACY_VOXELS);
      this.#session = { spacing: step > 0 ? step : f32(0.1), brushes: [] };
    });
  }
  smoothing(value) { return this.#run('smoothing', () => { const next = Math.max(0, f32(value)); this.#builder.require(LEGACY_VOXELS); this.#smoothing = next; }); }
  #operator(operator) {
    return this.#run(operator, () => {
      need(this.#session && this.#session.brushes.length > 0, 'postfix CSG requires a preceding brush in the current open voxel session');
      if (this.#session.brushes.length === 1 && operator !== 'union') fail('unsupported_capability', 'portable CSG requires the first brush to be union; retain the native path for first-brush subtraction/intersection');
      this.#session.brushes.at(-1).operator = operator;
    });
  }
  union() { return this.#operator('union'); }
  difference() { return this.#operator('subtract'); }
  intersection() { return this.#operator('intersect'); }
  endVoxels() {
    return this.#run('endVoxels', () => {
      need(this.#session && this.#session.brushes.length > 0, 'endVoxels requires a nonempty open voxel session');
      const session = this.#session;
      let result = session.brushes[0].reference;
      // Mixed union/subtract/intersect operations are an ordered left fold.
      for (const brush of session.brushes.slice(1)) result = this.#builder.csg(this.#key(), brush.operator, [result, brush.reference]);
      this.#voxelSessions.push({ output: result.reference, spacing_metres: session.spacing, smoothing_metres: this.#smoothing });
      this.#outputs.push(result); this.#session = null; return result;
    });
  }
  placeChild(module, params = {}, options = {}) {
    return this.#run('placeChild', () => {
      need(typeof module === 'string' && module.length > 0, 'child module must be an explicit source identity');
      fields(options);
      if (Object.keys(options).length) fail('unsupported_capability', 'legacy child renderer/inline policies need an explicit consumer adapter');
      if (typeof this.#options.resolveChild !== 'function') fail('missing_dependency', `resolve ${module} from the frozen dependency snapshot before authoring`);
      params = copyJson(params); fields(params);
      const request = freeze({ module, parameters: params });
      const pinned = this.#options.resolveChild(request);
      need(pinned && typeof pinned.then !== 'function', 'dependency discovery must finish before synchronous authoring');
      const dep = this.#builder.dependency(pinned), reference = this.#builder.assembly(this.#key(), dep);
      const placed = this.#builder.place(this.#key(), reference, this.#stack.at(-1));
      this.#outputs.push(placed); return placed;
    });
  }
  finish() {
    return this.#run('finish', () => {
      need(this.#stack.length === 1, 'pushMatrix without matching popMatrix');
      need(this.#session === null, 'beginVoxels without matching endVoxels');
      this.#sealed = true;
      if (this.#voxelSessions.length && !this.#extensionEmitted) {
        this.#builder.extension({ capability: LEGACY_VOXELS, required: true, payload: { sessions: this.#voxelSessions } });
        this.#extensionEmitted = true;
      }
      this.#builder.output(...this.#outputs);
      const document = this.#builder.build(); this.#finished = true; return document;
    });
  }
}
export function legacyPart(builder, options) {
  const part = new LegacyPart(builder, options);
  return new Proxy(part, { get(target, key) {
    // Do not masquerade as an async result when a host checks thenables.
    if (key === 'then') return undefined;
    const value = Reflect.get(target, key);
    if (typeof value === 'function') return value.bind(target);
    if (value !== undefined || typeof key !== 'string') return value;
    return (...args) => target.invoke(key, ...args);
  } });
}
