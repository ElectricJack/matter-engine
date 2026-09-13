// Explicit version-1 GPU field recipes, separate from legacy log-sum-exp CSG.
// Physical metres throughout. The catalogue contains eight ordinary bricks;
// placement reuses their geometry with rotation/translation, never scaling.
export const CASTLE_SOLID_SOURCE_SEEDS = Object.freeze([0, 1, 2, 3, 4, 5, 6, 7]);

export function castleStoneSourceSpec(p = {}) {
  const seed = p.seed ?? 0;
  if (!Number.isInteger(seed) || seed < 0 || seed > 7)
    throw new Error('CastleStoneSource seed must select one of eight recipes (0..7)');
  // Style zero preserves existing source recipes. The finished castle atlas
  // opts into broad chips; it still reuses the same eight physical bricks.
  const reliefStyle = p.reliefStyle ?? 0;
  if (reliefStyle !== 0 && reliefStyle !== 1)
    throw new Error('CastleStoneSource reliefStyle must be 0 (fine) or 1 (chunky)');
  const length = p.length ?? 0.30;
  const height = p.height ?? 0.14;
  const depth = p.depth ?? 0.20;
  const voxelM = p.voxelM ?? 0.003;
  if (![length, height, depth].every((n) => Number.isFinite(n) && n >= 0.04 && n <= 2))
    throw new Error('CastleStoneSource dimensions must be physical metres in [0.04,2]');
  if (!Number.isFinite(voxelM) || voxelM < 0.001 || voxelM > 0.006)
    throw new Error('CastleStoneSource voxelM must be in [0.001,0.006] metres');
  if (reliefStyle === 1 && Math.min(length, height, depth) < 0.10)
    throw new Error('chunky brick relief requires dimensions of at least 0.10m');
  let state = (0x9e3779b9 ^ Math.imul(seed + 1, 0x85ebca6b)) >>> 0;
  const random = () => {
    state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
    return state / 4294967296;
  };
  const roundingM = reliefStyle ? 0.005 : 0.0025;
  const ops = [{
    shape: 'roundedBox', combine: 'union',
    halfExtentsM: [length / 2 - roundingM, height / 2 - roundingM, depth / 2 - roundingM],
    centerM: [0, height / 2, 0], roundingM,
  }];
  // Shallow finite hollows on both broad faces. The physical relief and chip
  // size stay fixed when a separately authored brick has different dimensions.
  for (const side of [-1, 1]) {
    for (let i = 0; i < 9; ++i) {
      const radiusX = reliefStyle ? 0.018 + 0.022 * random() : 0.008 + 0.010 * random();
      const radiusY = reliefStyle ? 0.012 + 0.013 * random() : 0.006 + 0.006 * random();
      const radiusZ = reliefStyle ? 0.008 : 0.003;
      const penetration = reliefStyle ? 0.003 + 0.003 * random() : 0.0008 + 0.0012 * random();
      ops.push({
        shape: 'ellipsoid', combine: 'difference',
        radiiM: [radiusX, radiusY, radiusZ],
        centerM: [
          (random() - 0.5) * (length - 0.035),
          0.0175 + random() * (height - 0.035),
          side * (depth / 2 + radiusZ - penetration),
        ],
        blendM: reliefStyle ? 0.0012 : 0.0004,
      });
    }
  }
  for (let i = 0; i < 4; ++i) {
    const sideX = (i & 1) ? 1 : -1;
    const sideZ = (i & 2) ? 1 : -1;
    ops.push({
      shape: 'sphere', combine: 'difference',
      radiusM: reliefStyle ? 0.014 + 0.014 * random() : 0.008 + 0.009 * random(),
      centerM: [sideX * length / 2, (random() > 0.5 ? height : 0), sideZ * depth / 2],
    });
  }
  return { version: 1, voxelM, maxVertices: p.maxVertices ?? 500000, ops };
}

export function emitCastleStoneSource(part, p = {}) {
  part.fill(p.material ?? 8); // same caller-supplied limestone handles as CastleStone
  part.solidSource(castleStoneSourceSpec(p));
}
