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

// ---------------------------------------------------------------------------
// Token-based L-system + follower, ported from MatterEngine2's LSystem.cs and
// LSystemFollower.cs. Unlike `expand` above (single-char alphabet), this works
// on multi-character tokens ("Fwd", "Rotx1", "Leaf") separated by spaces, with
// "[" / "]" as their own bracket tokens. Tree/TreeBranch drive it.
// ---------------------------------------------------------------------------

// Split a rule/axiom string into tokens: whitespace separates, and "[" "]" are
// always standalone tokens even when adjacent to other characters.
export function tokenize(s) {
  const out = [];
  let i = 0;
  while (i < s.length) {
    const c = s[i];
    if (c === ' ' || c === '\t' || c === '\n') { i++; continue; }
    if (c === '[' || c === ']') { out.push(c); i++; continue; }
    let j = i;
    while (j < s.length && s[j] !== ' ' && s[j] !== '\t' && s[j] !== '\n'
           && s[j] !== '[' && s[j] !== ']') j++;
    out.push(s.slice(i, j));
    i = j;
  }
  return out;
}

export class LSystem {
  constructor() { this.rulesByMatch = {}; this.tokens = []; }

  // Define a rewrite rule "Match => a b [c] ...". Optional weight for stochastic
  // selection when several rules share a match token (defaults to 1).
  rule(spec, weight = 1) {
    const idx = spec.indexOf('=>');
    const match = spec.slice(0, idx).trim();
    const rewrite = tokenize(spec.slice(idx + 2));
    (this.rulesByMatch[match] = this.rulesByMatch[match] || []).push({ rewrite, weight });
    return this;
  }

  init(axiom) { this.tokens = tokenize(axiom); return this; }

  // Rewrite the current token list `iterations` times. When a match has several
  // weighted rules, pick one via Math.random() (seeded by the host bake).
  rewrite(iterations) {
    for (let it = 0; it < iterations; ++it) {
      const next = [];
      for (const tok of this.tokens) {
        const rules = this.rulesByMatch[tok];
        if (!rules) { next.push(tok); continue; }
        let chosen = rules[0];
        if (rules.length > 1) {
          const total = rules.reduce((a, r) => a + r.weight, 0);
          let pick = Math.random() * total;
          for (const r of rules) { pick -= r.weight; if (pick <= 0) { chosen = r; break; } }
        }
        for (const t of chosen.rewrite) next.push(t);
      }
      this.tokens = next;
    }
    return this;
  }
}

// Walks an LSystem's final token list, driving a turtle (the Part `part`) via its
// matrix stack. "[" / "]" push/pop matrix+distance, "Fwd" advances along +Y and
// records a skeleton segment, and named tokens fire registered actions.
//
// Hooks (all optional), matching v2:
//   onGetForwardDistance() -> number          step length for a Fwd
//   onBeginForward(t, dist, depth) -> bool     false = skip this Fwd (e.g. branch off)
//   onEndForward()                             jitter after a Fwd
//   onSegment(from, to, rFrom, rTo, depth)     a recorded tube/sweep segment
//
// Segment endpoints come from part.position() (absolute in the part's local
// frame), so callers should defer any geometry that bakes those coordinates
// until after execute() returns the turtle to the root matrix.
export class LSystemFollower {
  constructor(part, lsys, startDiameter, endDiameter, maxDistance) {
    this.part = part;
    this.tokens = lsys.tokens;
    this.startDiameter = startDiameter;
    this.endDiameter = endDiameter;
    this.maxDistance = maxDistance;
    this.currentDistance = 0;
    this.distStack = [];
    this.actions = {};
    this.onGetForwardDistance = null;
    this.onBeginForward = null;
    this.onEndForward = null;
    this.onSegment = null;
  }

  addAction(token, fn) { this.actions[token] = fn; }

  execute() {
    const p = this.part;
    p.pushMatrix();
    for (const tok of this.tokens) {
      if (tok === '[') {
        p.pushMatrix();
        this.distStack.push(this.currentDistance);
      } else if (tok === ']') {
        p.popMatrix();
        this.currentDistance = this.distStack.pop();
      } else if (tok === 'Fwd') {
        this.forward();
      } else {
        const a = this.actions[tok];
        if (a) a();
      }
    }
    p.popMatrix();
  }

  // Lerp diameter clamped to never drop below the end diameter (v2 Mathf.Max).
  width(t) {
    const w = this.startDiameter + (this.endDiameter - this.startDiameter) * t;
    return Math.max(w, this.endDiameter);
  }

  forward() {
    const p = this.part;
    const depth = this.distStack.length;
    let t = this.currentDistance / this.maxDistance;
    if (this.onBeginForward && !this.onBeginForward(t, this.currentDistance, depth)) return;

    const rFrom = this.width(t);
    const from = p.position();

    const dist = this.onGetForwardDistance
      ? this.onGetForwardDistance()
      : (0.5 + Math.random() * 0.5);
    p.translate(0, dist, 0);
    this.currentDistance += dist;

    t = this.currentDistance / this.maxDistance;
    const rTo = this.width(t);
    const to = p.position();

    if (this.onSegment) this.onSegment(from, to, rFrom, rTo, depth);
    if (this.onEndForward) this.onEndForward();
  }
}
