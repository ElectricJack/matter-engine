// Thin wrappers over the __pf_* native bindings so schemas get an ergonomic,
// documented API. All heavy state lives in C++; these classes only hold ids.

export class PathRecorder {
  // channels: array of attribute names to record per-vertex (e.g. ['thickness'])
  constructor(minSegment, channels) {
    this.__id = __pf_recorderCreate(minSegment, channels || []);
  }
  get count() { return __pf_pathCount(this.__id); }
  path(i) { return __pf_path(this.__id, i); }         // copied {xyz, channels,...}
  forEach(fn) { const n = this.count; for (let i = 0; i < n; ++i) fn(this.path(i), i); }
}

export class ParticleSim {
  // cfg: see __pf_simCreate — {seed, dt, maxTurnRate, speedTarget, speedRelax,
  //   depositEvery, maxAge, maxParticles, hashCell, attributes: [names],
  //   state: [names],   // per-particle script-state floats: zero-init, NOT
  //                     // path-recorded, writable via onTick view.state.<name>
  //   emitters: [{shape:'point'|'disc'|'ring', center, axis, radius, rate,
  //               vel0, jitter, attrInit, stateInit}],
  //   fields: [{type:'bias'|'curl'|'adhere'|'attract'|'separate'|'drag',
  //             mode:'steer'|'force', weight, dir, radius, surfaceOffset,
  //             influence, killRadius, killOnConsume, scale, seed, k,
  //             weightState: 'name',  // per-particle weight multiplier from a
  //                                   // state channel (clamped >= 0)
  //             fade:{axis:'x'|'y'|'z', from, to}}]}
  constructor(cfg) { this.__id = __pf_simCreate(cfg); }
  attach(recorder) { __pf_attach(this.__id, recorder.__id); return this; }
  setAttractors(f32) { __pf_setAttractors(this.__id, f32); return this; }
  // run(ticks) or run(ticks, every, onTick) — onTick(view) may return false to stop.
  // view: {count, tick, pos, vel, alive, attrs:{name: F32}, state:{name: F32}}
  run(ticks, every, onTick) { return __pf_run(this.__id, ticks, every, onTick); }
  emit(cfg) { __pf_emit(this.__id, cfg); }
  kill(slot) { __pf_kill(this.__id, slot); }
  setFieldWeight(i, w) { __pf_setFieldWeight(this.__id, i, w); }
  get attractorsRemaining() { return __pf_attractorsRemaining(this.__id); }
  get depositedCount() { return __pf_depositedCount(this.__id); }
  surfaceNormal(p, radius) { return __pf_surfaceNormal(this.__id, p[0], p[1], p[2], radius); }
  // Nearest live attractor within maxDist -> {idx, pos:[x,y,z], dist} | null.
  nearestAttractor(p, maxDist, unclaimedOnly) {
    return __pf_nearestAttractor(this.__id, p[0], p[1], p[2], maxDist, !!unclaimedOnly);
  }
  // Reserve an attractor for a particle slot: generic attract steering skips
  // it, the claimer beelines to it. Released if the claimer dies unconsumed.
  claimAttractor(slot, idx) { return __pf_claimAttractor(this.__id, slot, idx); }
  // Count of deposited (wood) points within radius — open-space probe.
  depositNear(p, radius) { return __pf_depositNear(this.__id, p[0], p[1], p[2], radius); }
}
