// Host-independent reference for matter-exact-json-v1 and matter.splitmix64.v1.
// No Node, DOM, filesystem, network or geometry/runtime imports.
const encoder = new TextEncoder();
function validUnicode(text) {
  for (let i = 0; i < text.length; ++i) {
    const c = text.charCodeAt(i);
    if (c >= 0xd800 && c <= 0xdbff) {
      const next = text.charCodeAt(++i);
      if (!(next >= 0xdc00 && next <= 0xdfff)) throw new TypeError('unpaired surrogate');
    } else if (c >= 0xdc00 && c <= 0xdfff) throw new TypeError('unpaired surrogate');
  }
}
function quote(text, maxStringBytes) {
  validUnicode(text);
  if (encoder.encode(text).length > maxStringBytes) throw new RangeError('string budget');
  let out = '"';
  for (const c of text) {
    const n = c.codePointAt(0);
    if (c === '"') out += '\\"';
    else if (c === '\\') out += '\\\\';
    else if (n < 32) out += `\\u${n.toString(16).padStart(4, '0')}`;
    else out += c;
  }
  return out + '"';
}
function utf8Compare(a, b) {
  const left = encoder.encode(a), right = encoder.encode(b);
  for (let i = 0; i < Math.min(left.length, right.length); ++i) {
    if (left[i] !== right[i]) return left[i] - right[i];
  }
  return left.length - right.length;
}
export function exactNumber(n) {
  if (!Number.isFinite(n)) throw new TypeError('finite binary64 required');
  if (n === 0) return '0';
  const view = new DataView(new ArrayBuffer(8));
  view.setFloat64(0, n, false);
  const bits = view.getBigUint64(0, false);
  const negative = bits >> 63n;
  const encoded = Number((bits >> 52n) & 2047n);
  let mantissa = bits & 0xfffffffffffffn;
  let exponent = -1074;
  if (encoded) { mantissa |= 0x10000000000000n; exponent = encoded - 1023 - 52; }
  let digits = (mantissa * (exponent < 0 ? 5n : 2n) ** BigInt(Math.abs(exponent))).toString();
  if (exponent < 0) {
    const places = -exponent;
    digits = digits.padStart(places + 1, '0');
    digits = digits.slice(0, -places) + '.' + digits.slice(-places);
    digits = digits.replace(/0+$/, '').replace(/\.$/, '');
  }
  return (negative ? '-' : '') + digits;
}
export function canonicalJson(value, limits = {}) {
  const maxDepth = limits.maxDepth ?? 128;
  const maxNodes = limits.maxNodes ?? 1000000;
  const maxBytes = limits.maxBytes ?? 64 * 1024 * 1024;
  const maxStringBytes = limits.maxStringBytes ?? 65536;
  const chunks = [];
  let nodes = 0, bytes = 0;
  const active = new Set();
  function emit(text) {
    bytes += encoder.encode(text).length;
    if (bytes > maxBytes) throw new RangeError('document byte budget');
    chunks.push(text);
  }
  function visit(v, depth) {
    if (depth > maxDepth || ++nodes > maxNodes) throw new RangeError('value budget');
    if (v === null) { emit('null'); return; }
    if (typeof v === 'boolean') { emit(v ? 'true' : 'false'); return; }
    if (typeof v === 'number') { emit(exactNumber(v)); return; }
    if (typeof v === 'string') { emit(quote(v, maxStringBytes)); return; }
    if (typeof v !== 'object') throw new TypeError('JSON value required');
    if (active.has(v)) throw new TypeError('cyclic value');
    active.add(v);
    if (Array.isArray(v)) {
      emit('[');
      for (let i = 0; i < v.length; ++i) { if (i) emit(','); visit(v[i], depth + 1); }
      emit(']');
    } else {
      const prototype = Object.getPrototypeOf(v);
      if (prototype !== Object.prototype && prototype !== null) throw new TypeError('plain object required');
      emit('{');
      const keys = Object.keys(v).sort(utf8Compare);
      for (let i = 0; i < keys.length; ++i) {
        if (i) emit(','); emit(quote(keys[i], maxStringBytes)); emit(':'); visit(v[keys[i]], depth + 1);
      }
      emit('}');
    }
    active.delete(v);
  }
  visit(value, 0);
  return chunks.join('');
}
export function float32(n) {
  if (!Number.isFinite(n)) throw new TypeError('finite float32 required');
  const value = Math.fround(n);
  if (!Number.isFinite(value)) throw new RangeError('float32 overflow');
  return value;
}
export function hex64(n) {
  if (typeof n !== 'bigint' || n < 0n || n > 0xffffffffffffffffn) throw new RangeError('uint64 required');
  return n.toString(16).padStart(16, '0');
}
export class Rng {
  constructor(seed) {
    hex64(seed);
    this.state = seed || 0x9e3779b97f4a7c15n;
  }
  nextU64() {
    this.state = BigInt.asUintN(64, this.state + 0x9e3779b97f4a7c15n);
    let z = this.state;
    z = BigInt.asUintN(64, (z ^ (z >> 30n)) * 0xbf58476d1ce4e5b9n);
    z = BigInt.asUintN(64, (z ^ (z >> 27n)) * 0x94d049bb133111ebn);
    return z ^ (z >> 31n);
  }
  nextUnit() { return Number(this.nextU64() >> 11n) / 9007199254740992; }
}
// Input is a structurally/semantically validated, float32-normalized document.
// Inject digestBytes(bytes) -> Promise<Uint8Array> for hosts without Web Crypto.
export async function contentDigest(document, digestBytes = async bytes =>
  new Uint8Array(await globalThis.crypto.subtle.digest('SHA-256', bytes))) {
  if (document.canonical_profile !== 'matter-exact-json-v1') throw new TypeError('unsupported canonical profile');
  const semantic = { ...document };
  delete semantic.metadata;
  delete semantic.source_map;
  semantic.extensions = semantic.extensions.filter(extension => extension.required);
  const bytes = encoder.encode('matter-exact-json-v1\n' + canonicalJson(semantic));
  const digest = await digestBytes(bytes);
  if (!(digest instanceof Uint8Array) || digest.length !== 32) throw new TypeError('SHA-256 bytes required');
  return { algorithm: 'sha256', hex: Array.from(digest, x => x.toString(16).padStart(2, '0')).join('') };
}
