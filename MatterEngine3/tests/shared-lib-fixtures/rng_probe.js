import { rng } from '../../shared-lib/rng.js';
const r = rng(42);
console.log([r.next(), r.next(), r.next(), r.next()].join(' '));
