import { HABITAT } from 'shared-lib/alpine_ecology';

export const MOUNTAIN_FOREST_PROFILE = 'shared-evergreen-forest';
export const MOUNTAIN_FOREST_SPACING = 8.5;
export const MOUNTAIN_FOREST_MIN_LOD = 2;
const clamp = x => Math.max(0, Math.min(1, x));
const fade = (a, b, x) => { const t = clamp((x - a) / (b - a)); return t*t*(3-2*t); };

// Four immutable assemblies: Scots pine, silver fir, coast redwood and a
// three-stem redwood base. Scatter varies poses, never child bake parameters.
export function mountainTreeParams(p) {
  const materials = { barkMaterial:p.barkMaterial, branchMaterial:p.branchMaterial,
    needleMaterial:p.needleMaterial, coneMaterial:p.coneMaterial };
  if (p.kind >= 2) materials.barkMaterial = p.redwoodMaterial;
  const variants = [
    { species:0, seed:42 },
    { species:1, seed:63, age:46, height:16, dbh:0.38, crownRatio:0.88,
      crownRadius:3.4, whorlCount:40, branchesPerWhorl:7, droop:0.12,
      fullness:1.4, coneDensity:0.8 },
    { species:2, seed:137, age:220, height:62, dbh:2.1, crownRadius:6,
      crownRatio:0.70, whorlCount:48, branchesPerWhorl:4, fullness:1.25,
      branchLoss:0.10, lean:0.014, droop:0.18, coneDensity:0.7 },
    { seed:91, stemCount:3, height:48, stemSpread:1.15, heightVariation:0.16 },
  ];
  return { ...materials, ...variants[Math.max(0, Math.min(3, p.kind|0))] };
}

export function mountainForestCatalog(materials = {}) {
  const defaults = { barkMaterial:14, branchMaterial:14, needleMaterial:29,
    coneMaterial:14, redwoodMaterial:14, ...materials };
  return [0,1,2,3].map(kind => ({ module:'MountainEvergreen', params:{ kind, ...defaults } }));
}

export function planMountainForest({ worldSeed, ox, oz, sectorSize=64,
    candidatesInRect, habitatAt, biomeAt, materials }) {
  if (!habitatAt) return [];
  const kit = mountainForestCatalog(materials), out = [], sample = [];
  const candidates = candidatesInRect(worldSeed >>> 0, 0xC041, MOUNTAIN_FOREST_SPACING,
    ox, oz, sectorSize, sectorSize);
  for (const c of candidates) {
    if (c.x < ox || c.x >= ox+sectorSize || c.z < oz || c.z >= oz+sectorSize || biomeAt(c.x,c.z)==='ocean') continue;
    habitatAt(c.x,c.z,sample);
    const height=sample[HABITAT.altitude], slope=sample[HABITAT.slope], moisture=sample[HABITAT.moisture];
    if (![height,slope,moisture,sample[HABITAT.forest],sample[HABITAT.forestEdge]].every(Number.isFinite)) continue;
    // Keep summit snow, cliffs and open meadows clear; taper the treeline.
    const forest=clamp(sample[HABITAT.forest]), edge=clamp(sample[HABITAT.forestEdge]);
    const density=(0.88*forest+0.12*edge)*(1-fade(0.43,0.64,slope))*(1-fade(310,440,height));
    if (c.u >= density || height < 0) continue;
    const wetGrove=height<155 && moisture>0.56 && forest>0.5;
    const choice=(c.v*17.731)%1;
    const kind=wetGrove && choice<0.22 ? (choice<0.055?3:2) :
      choice < 0.30+moisture*0.45 ? 1 : 0;
    const scale=0.88+0.24*((c.v*43.217)%1);
    out.push({ ...kit[kind], params:{...kit[kind].params}, kind,
      x:c.x, z:c.z, rotation:c.rot, scale, sinkY:0.12*scale });
  }
  return out;
}
