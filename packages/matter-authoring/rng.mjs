import { PROFILE, RNG_ALGORITHM, fail, need, seed64, integer, finite, freeze } from './values.mjs';

// Adapt the frozen shared contract helper (or an equivalent injected host service).
// No copied RNG/canonical implementation and no default Web Crypto dependency.
export function contractServices({ canonicalJson, Rng, contentDigest }, digestBytes) {
  need(typeof canonicalJson === 'function' && typeof Rng === 'function', 'canonicalJson and contract Rng services are required');
  return Object.freeze({ canonicalProfile: PROFILE, rngAlgorithm: RNG_ALGORITHM,
    canonicalJson, createRng: seed => new Rng(seed),
    digestDocument: typeof contentDigest === 'function' && typeof digestBytes === 'function'
      ? document => contentDigest(document, digestBytes) : undefined });
}
export class SeededRng {
  #engine; #services; #draws = 0;
  constructor(seed, services) {
    this.#services = services;
    if (services?.rngAlgorithm !== RNG_ALGORITHM || typeof services.createRng !== 'function')
      fail('unsupported_capability', `inject ${RNG_ALGORITHM} contract services to construct a seeded RNG`);
    this.#engine = services.createRng(seed64(seed));
    need(this.#engine && typeof this.#engine.nextU64 === 'function', 'RNG service must return nextU64() and explicit uint64 state');
    seed64(this.#engine.state);
  }
  nextU64() { const word = seed64(this.#engine.nextU64()); ++this.#draws; return word; }
  random() { return Number(this.nextU64() >> 11n) / 9007199254740992; }
  nextUnit() { return this.random(); }
  int(n) { integer(n, 1, Number.MAX_SAFE_INTEGER, 'RNG integer bound'); return Math.floor(this.random() * n); }
  range(a, b) {
    finite(a); finite(b); need(a <= b && Number.isFinite(b - a), 'RNG range must be finite and ordered');
    return finite(a + this.random() * (b - a));
  }
  snapshot() { return freeze({ algorithm: RNG_ALGORITHM, state: seed64(this.#engine.state).toString(16).padStart(16, '0'), draws: this.#draws }); }
  restore(snapshot) {
    need(snapshot?.algorithm === RNG_ALGORITHM, 'incompatible RNG snapshot');
    const state = seed64(snapshot.state), draws = integer(snapshot.draws, 0, Number.MAX_SAFE_INTEGER);
    this.#engine.state = state; this.#draws = draws; return this;
  }
  clone() { return new SeededRng(1n, this.#services).restore(this.snapshot()); }
}
