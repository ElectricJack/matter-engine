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

export function candidatesInRect(seed, kind, minDist, x0, z0, w, h) {
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
