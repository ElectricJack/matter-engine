// Only ECMAScript values and explicitly supplied services cross this boundary.
export const PROFILE = 'matter-exact-json-v1';
export const RNG_ALGORITHM = 'matter.splitmix64.v1';
export class AuthoringError extends Error {
  constructor(code, message, context = {}) {
    const location = context.source;
    super(`${code}: ${message}${location ? ` (${location.uri}:${location.line}:${location.column})` : ''}`);
    this.name = 'AuthoringError';
    this.diagnostic = Object.freeze({ severity: 'error', code, message,
      node_id: context.node_id ?? '', source: location ? Object.freeze({ ...location }) : null,
      dependency_path: Object.freeze([...(context.dependency_path ?? [])]) });
  }
}
export function fail(code, message, context) { throw new AuthoringError(code, message, context); }
export function need(condition, message, context) { if (!condition) fail('invalid_input', message, context); }
export function unicode(value, label = 'string', maxBytes = 65536) {
  need(typeof value === 'string', `${label} must be a string`);
  let bytes = 0;
  for (let i = 0; i < value.length; ++i) {
    const c = value.charCodeAt(i);
    if (c >= 0xd800 && c <= 0xdbff) {
      const next = value.charCodeAt(++i);
      need(next >= 0xdc00 && next <= 0xdfff, `${label} has an unpaired surrogate`);
      bytes += 4;
    } else need(c < 0xdc00 || c > 0xdfff, `${label} has an unpaired surrogate`);
    if (c < 0xd800 || c > 0xdbff) bytes += c < 128 ? 1 : c < 2048 ? 2 : 3;
    if (bytes > maxBytes) fail('resource_limit', `${label} exceeds UTF-8 byte budget`);
  }
  return value;
}
export function id(value) {
  need(typeof value === 'string' && /^[A-Za-z0-9][A-Za-z0-9._:/-]{0,255}$/.test(value), 'invalid semantic ID');
  return value;
}
export function finite(value, label = 'number') {
  need(typeof value === 'number' && Number.isFinite(value), `${label} must be finite`);
  return Object.is(value, -0) ? 0 : value;
}
export function f32(value) { return finite(Math.fround(finite(value)), 'float32'); }
export function integer(value, min = 0, max = 0xffffffff, label = 'integer') {
  need(Number.isSafeInteger(value) && value >= min && value <= max, `${label} must be an integer in [${min},${max}]`);
  return value;
}
export function seed64(value) {
  if (typeof value === 'string' && /^[0-9a-f]{16}$/.test(value)) return BigInt(`0x${value}`);
  need(typeof value === 'bigint' && value >= 0n && value <= 0xffffffffffffffffn,
    'seed/count must be uint64 BigInt or 16 lowercase hexadecimal digits; Number loses precision');
  return value;
}
export function hex64(value) { return seed64(value).toString(16).padStart(16, '0'); }
export function vector(value, length, normalize = finite) {
  need(Array.isArray(value) && value.length === length, `expected ${length} components`);
  return value.map(normalize);
}
export function fields(value, allowed, required = allowed) {
  need(value !== null && typeof value === 'object' && !Array.isArray(value), 'expected object');
  if (allowed === undefined) return;
  need(Object.keys(value).every(key => allowed.includes(key)), 'unknown structural field');
  need(required.every(key => Object.hasOwn(value, key)), 'missing structural field');
}
// Unicode scalar order is also unsigned UTF-8 byte order, without a TextEncoder.
export function compareKeys(a, b) {
  const left = Array.from(a, c => c.codePointAt(0)), right = Array.from(b, c => c.codePointAt(0));
  for (let i = 0; i < Math.min(left.length, right.length); ++i) if (left[i] !== right[i]) return left[i] - right[i];
  return left.length - right.length;
}
export function copyJson(value, { maxDepth = 128, maxValues = 1000000, maxStringBytes = 65536 } = {}) {
  const active = new Set(); let count = 0;
  function visit(v, depth) {
    if (depth > maxDepth || ++count > maxValues) fail('resource_limit', 'JSON depth/value budget exceeded');
    if (v === null || typeof v === 'boolean') return v;
    if (typeof v === 'number') return finite(v);
    if (typeof v === 'string') return unicode(v, 'string', maxStringBytes);
    need(v !== null && typeof v === 'object', 'recipe values must be plain JSON, without closures/undefined/BigInt');
    if (active.has(v)) fail('cycle', 'cyclic parameter value');
    active.add(v);
    need(Object.getOwnPropertySymbols(v).length === 0, 'symbol keys are not JSON');
    const result = Array.isArray(v) ? [] : {};
    need(Array.isArray(v) || Object.getPrototypeOf(v) === Object.prototype || Object.getPrototypeOf(v) === null,
      'plain JSON object required');
    if (Array.isArray(v) && v.length > maxValues - count) fail('resource_limit', 'JSON array exceeds value budget');
    const keys = Array.isArray(v) ? Array.from({ length: v.length }, (_, i) => String(i)) : Object.keys(v).sort(compareKeys);
    if (Array.isArray(v)) need(Object.keys(v).length === v.length, 'dense JSON array without extra properties required');
    for (const key of keys) {
      unicode(key, 'key', maxStringBytes);
      const descriptor = Object.getOwnPropertyDescriptor(v, key);
      need(descriptor && Object.hasOwn(descriptor, 'value') && descriptor.enumerable, 'JSON accessors/array holes are forbidden');
      Object.defineProperty(result, key, { value: visit(descriptor.value, depth + 1), enumerable: true, writable: true, configurable: true });
    }
    active.delete(v); return result;
  }
  return visit(value, 0);
}
export function freeze(value) {
  if (value && typeof value === 'object' && !Object.isFrozen(value)) {
    for (const child of Object.values(value)) freeze(child);
    Object.freeze(value);
  }
  return value;
}
export function location(value) {
  fields(value, ['uri', 'line', 'column']);
  return { uri: unicode(value.uri), line: integer(value.line), column: integer(value.column) };
}
export function digest(value) {
  fields(value, ['algorithm', 'hex']);
  need(value.algorithm === 'sha256' && /^[0-9a-f]{64}$/.test(value.hex), 'expected SHA-256 digest');
  return { ...value };
}
export function version(value) {
  fields(value, ['major', 'minor']);
  if (value.major !== 1 || value.minor !== 0) fail('unsupported_version', 'only contract schema 1.0 is supported');
  return { major: 1, minor: 0 };
}
export function capability(value) {
  fields(value, ['name', 'version']);
  return { name: id(value.name), version: integer(value.version, 1) };
}
export const DEFAULT_CAPABILITIES = freeze([{ name: 'matter.recipe.core', version: 1 }, { name: 'matter.material.pbr', version: 1 }]);
export function checkCapability(value, supported, context) {
  capability(value);
  if (!supported.some(c => c.name === value.name && c.version === value.version))
    fail('unsupported_capability', `required ${value.name}@${value.version} is unavailable; supply a host/evaluator with this capability`, context);
}
export function identity() { return [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]; }
export function affine(value, similarity = false, context) {
  const m = vector(value, 16, f32);
  need(m[12] === 0 && m[13] === 0 && m[14] === 0 && m[15] === 1, 'affine matrix last row must be [0,0,0,1]', context);
  const columns = [0, 1, 2].map(i => [m[i], m[4 + i], m[8 + i]]);
  const dot = (a, b) => a.reduce((sum, x, i) => sum + x * b[i], 0);
  const lengths = columns.map(c => Math.sqrt(dot(c, c)));
  const det = m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) + m[2] * (m[4] * m[9] - m[5] * m[8]);
  need(lengths.every(x => x > 0) && Math.abs(det) > 1e-12 * lengths[0] * lengths[1] * lengths[2], 'singular/near-dependent affine transform', context);
  if (similarity) {
    const squares = lengths.map(x => x * x), tolerance = 1e-5 * Math.max(...squares);
    if (det <= 0 || squares.some(x => Math.abs(x - squares[0]) > tolerance) ||
      Math.abs(dot(columns[0], columns[1])) > tolerance || Math.abs(dot(columns[0], columns[2])) > tolerance || Math.abs(dot(columns[1], columns[2])) > tolerance)
      fail('unsupported_capability', 'CSG v1 accepts rotation, translation and positive uniform scale; use full affine placement outside CSG', context);
  }
  return m;
}
export function multiply(parent, local) {
  const a = affine(parent), b = affine(local), out = [];
  // MathLib's float arithmetic rounds each multiplication and addition.
  for (let row = 0; row < 4; ++row) for (let col = 0; col < 4; ++col) {
    let sum = 0;
    for (let k = 0; k < 4; ++k) sum = f32(sum + f32(a[row * 4 + k] * b[k * 4 + col]));
    out.push(sum);
  }
  return affine(out);
}
export function translation(x, y, z) { const m = identity(); [m[3], m[7], m[11]] = [x, y, z].map(f32); return m; }
export function scale(x, y, z) { const m = identity(); [m[0], m[5], m[10]] = [x, y, z].map(f32); return affine(m); }
export function rotation(axis, radians) {
  need(['x', 'y', 'z'].includes(axis), 'rotation axis must be x/y/z');
  const c = f32(Math.cos(f32(radians))), s = f32(Math.sin(f32(radians))), m = identity();
  if (axis === 'x') { m[5] = c; m[6] = -s; m[9] = s; m[10] = c; }
  if (axis === 'y') { m[0] = c; m[2] = s; m[8] = -s; m[10] = c; }
  if (axis === 'z') { m[0] = c; m[1] = -s; m[4] = s; m[5] = c; }
  return affine(m);
}
