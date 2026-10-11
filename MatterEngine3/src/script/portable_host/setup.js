// The shared contract helper expects TextEncoder. Capture the native encoder
// and remove its temporary global before any supplied source executes.
((encode) => {
  class TextEncoder {
    encode(value = '') { return encode(String(value)); }
    get encoding() { return 'utf-8'; }
  }
  Object.freeze(TextEncoder.prototype);
  Object.freeze(TextEncoder);
  Object.defineProperty(globalThis, 'TextEncoder', { value: TextEncoder });
})(globalThis.__matterUtf8);
delete globalThis.__matterUtf8;
// Vendored QuickJS 0.10.0 normalizes a truncated unsigned BigInt limb as signed:
// asUintN(64, 0xffffffffffffffffn) returns -1n. Correct its unsigned result at
// this host boundary; keep the shared SplitMix64 implementation untouched.
// Unary + performs ToNumber once (including rejection of BigInt widths), and
// the native operation still performs ToIndex/ToBigInt and their validation.
((asUintN, truncate) => {
  Object.defineProperty(BigInt, 'asUintN', { value(bits, value) {
    const width = +bits;
    const result = asUintN(width, value);
    return result < 0n ? result + (1n << BigInt(truncate(width))) : result;
  }, writable: false, configurable: false });
})(BigInt.asUintN, Math.trunc);
Object.defineProperty(Math, 'random', { value() { throw new TypeError('use context.services seeded RNG'); }, writable: false, configurable: false });
for (const name of ['Date', 'SharedArrayBuffer', 'Atomics'])
  Object.defineProperty(globalThis, name, { value: undefined, writable: false, configurable: false });
// The same vendor build includes the positive sign limb in binary-radix
// strings (UINT64_MAX.toString(16) starts with '0'). Remove only redundant
// leading zeroes, preserving native receiver/radix validation and sign.
((toString) => {
  Object.defineProperty(BigInt.prototype, 'toString', { value(radix) {
    return toString.call(this, radix).replace(/^(-?)0+(?=.)/, '$1');
  }, writable: false, configurable: false });
})(BigInt.prototype.toString);
