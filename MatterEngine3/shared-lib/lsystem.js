// L-system string rewriting. Pure given (axiom, rules, iterations). Stochastic
// rules (value = array of {to, weight}) draw from a seeded rng for determinism.
import { rng } from 'shared-lib/rng';

export function expand(axiom, rules, iterations, seed = 0) {
  const r = rng(seed);
  let s = axiom;
  for (let it = 0; it < iterations; ++it) {
    let next = '';
    for (const ch of s) {
      const rule = rules[ch];
      if (rule === undefined) { next += ch; continue; }
      if (typeof rule === 'string') { next += rule; continue; }
      // stochastic: pick by weight using seeded rng
      const total = rule.reduce((a, o) => a + o.weight, 0);
      let pick = r.random() * total;
      for (const o of rule) { pick -= o.weight; if (pick <= 0) { next += o.to; break; } }
    }
    s = next;
  }
  return s;
}
