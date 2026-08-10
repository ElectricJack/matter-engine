// Deterministic spaced scatter on a virtual world grid. One candidate per
// minDist-sized cell; neighbor-priority rejection gives an exact min-dist
// guarantee that is order-independent and identical from any sector.

function mix(h, c) {
  h = Math.imul(h ^ (h >>> 15), c | 1);
  h ^= h + Math.imul(h ^ (h >>> 7), h | 61);
  return (h ^ (h >>> 14)) >>> 0;
}
function baseHash(seed, kind, cx, cz) {
  let h = (seed | 0) ^ Math.imul(kind | 0, 374761393);
  h = mix(h ^ Math.imul(cx | 0, 668265263), 2246822519);
  h = mix(h ^ Math.imul(cz | 0, 1274126177), 374761393);
  return h >>> 0;
}
const unit = h => h / 4294967296;   // [0,1)

export function cellCandidate(seed, kind, cellX, cellZ, minDist) {
  const h = baseHash(seed, kind, cellX, cellZ);
  const jx = unit(mix(h, 0x9E3779B1)), jz = unit(mix(h, 0x85EBCA77));
  return {
    x: (cellX + 0.25 + 0.5 * jx) * minDist,
    z: (cellZ + 0.25 + 0.5 * jz) * minDist,
    rot: unit(mix(h, 0xC2B2AE3D)) * Math.PI * 2,
    u: unit(mix(h, 0x27D4EB2F)),
    v: unit(mix(h, 0x165667B1)),
    pri: h,
  };
}

export function survives(seed, kind, cellX, cellZ, minDist) {
  const c = cellCandidate(seed, kind, cellX, cellZ, minDist);
  for (let dz = -1; dz <= 1; ++dz)
    for (let dx = -1; dx <= 1; ++dx) {
      if (dx === 0 && dz === 0) continue;
      const nx = cellX + dx, nz = cellZ + dz;
      const o = cellCandidate(seed, kind, nx, nz, minDist);
      const ddx = c.x - o.x, ddz = c.z - o.z;
      if (ddx * ddx + ddz * ddz >= minDist * minDist) continue;
      // Conflict: higher priority wins; ties broken by cell coords (lexicographic).
      if (o.pri > c.pri) return false;
      if (o.pri === c.pri && (nz < cellZ || (nz === cellZ && nx < cellX))) return false;
    }
  return true;
}

// The reference implementation, kept and still exported as candidatesInRectJs
// so the native binding can be proved against it rather than believed. Do not
// delete it to "remove the duplicate": it is the specification, and the whole
// safety argument for the native path is that the two agree bit for bit.
export function candidatesInRectJs(seed, kind, minDist, x0, z0, w, h) {
  const out = [];
  const c0 = Math.floor(x0 / minDist), c1 = Math.floor((x0 + w) / minDist);
  const r0 = Math.floor(z0 / minDist), r1 = Math.floor((z0 + h) / minDist);
  for (let cz = r0; cz <= r1; ++cz)
    for (let cx = c0; cx <= c1; ++cx) {
      const c = cellCandidate(seed, kind, cx, cz, minDist);
      if (c.x < x0 || c.x >= x0 + w || c.z < z0 || c.z >= z0 + h) continue;
      if (!survives(seed, kind, cx, cz, minDist)) continue;
      out.push({ x: c.x, z: c.z, rot: c.rot, u: c.u, v: c.v });
    }
  return out;
}

// The one every caller uses. Native when the binding is installed (part bakes),
// the JS above otherwise (contexts with no DSL bindings, and any host predating
// the binding).
//
// WHY IT WENT NATIVE. Measured with ScriptProfile on StreamMountain, this was
// the single largest cost inside a sector bake -- 46.6% of profiled self time
// at the far vegetation band, ahead of habitat sampling, asset selection, the
// exclusion pass, and the native terrain mesher. The reason is structural: the
// loop above evaluates cellCandidate TEN times per grid cell (the cell, plus
// eight neighbours inside survives, plus the cell again inside survives), each
// allocating an object that is read once. A 96 m tree rect at 1.65 m spacing is
// 59x59 cells -- ~35,000 allocations and ~244,000 hash rounds to return ~1,300
// candidates. Grass, at 0.63 m spacing, is denser still.
//
// It is a pure function of (seed, kind, minDist, rect) with nothing
// game-specific in it, which is why it belongs to the engine and not to an
// ecology.
//
// The native path is bitwise identical, not merely equivalent -- every
// placement in every world derives from these hashes, so "close enough" would
// move trees. sector_bake_tests proves the two agree over the real rects.
export function candidatesInRect(seed, kind, minDist, x0, z0, w, h) {
  if (typeof __candidatesInRect === 'function')
    return __candidatesInRect(seed, kind, minDist, x0, z0, w, h);
  return candidatesInRectJs(seed, kind, minDist, x0, z0, w, h);
}

// The JS twin of dsl_bindings.cpp's j_planCandidates (__planCandidates) --
// the same delegation shape as candidatesInRect above, one level up: a caller
// with no native fused binding (the alpine_ecology_tests.mjs Node harness,
// which has no QuickJS bindings at all) still gets ONE producer to consume,
// not a second hand-rolled candidates-then-habitat loop next to the fused one.
//
// Layout matches the native binding exactly: a flat Float64Array where
// data[0]=channelCount, data[1]=candidateCount, and record i sits at
// 2 + i*(5+channelCount) as [x, z, rot, u, v, ...channels]. `candidatesInRect`
// is the candidate-grid function to fuse -- ordinarily candidatesInRectJs
// itself, but callers may pass any function with the same signature (a test
// stub, say) and this still fuses against it, exactly as the engine binding
// fuses __candidatesInRect's C++ loop against __habitatAt's. `habitatAt(x, z,
// out)` fills `out` with `channelCount` channel values, the same shape as the
// engine's __habitatAt. Pass channelCount = 0 (habitatAt then unused) for the
// "no habitat tape bound" case -- the same contract __planCandidates uses.
export function planCandidatesJs(
  candidatesInRect, seed, kind, minDist, x0, z0, w, h, habitatAt, channelCount,
) {
  const candidates = candidatesInRect(seed, kind, minDist, x0, z0, w, h);
  const count = candidates.length;
  const stride = 5 + channelCount;
  const flat = new Float64Array(2 + count * stride);
  flat[0] = channelCount;
  flat[1] = count;
  const scratch = channelCount > 0 ? new Array(channelCount) : null;
  for (let i = 0; i < count; ++i) {
    const c = candidates[i];
    const base = 2 + i * stride;
    flat[base] = c.x; flat[base + 1] = c.z; flat[base + 2] = c.rot;
    flat[base + 3] = c.u; flat[base + 4] = c.v;
    if (channelCount > 0) {
      habitatAt(c.x, c.z, scratch);
      for (let ch = 0; ch < channelCount; ++ch) flat[base + 5 + ch] = scratch[ch];
    }
  }
  return flat;
}
