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
Object.defineProperty(Math, 'random', { value() { throw new TypeError('use context.services seeded RNG'); }, writable: false, configurable: false });
for (const name of ['Date', 'SharedArrayBuffer', 'Atomics'])
  Object.defineProperty(globalThis, name, { value: undefined, writable: false, configurable: false });
