// Stable scalar lookups shared by world roots and independently baked Parts.
import { decorateCastleWingPlan } from 'shared-lib/castle_wing_interiors';
import { compilePlan } from 'shared-lib/castle_plan';
import { castleSitePlan, CASTLE_SITE_NAMES, CASTLE_SITE_SEEDS } from 'shared-lib/castle_site_programs';

export { CASTLE_SITE_NAMES, CASTLE_SITE_SEEDS };
const plans=new Map(),manifests=new Map();
export function castleSiteProgram(variant=0,seed=CASTLE_SITE_SEEDS[variant]) {
 if(!Number.isInteger(variant)||!CASTLE_SITE_NAMES[variant])throw new Error('Invalid castle site variant '+variant);
 const key=variant+':'+seed;
 if(!plans.has(key)){
  const site=castleSitePlan(CASTLE_SITE_NAMES[variant],seed);
  for(const wing of site.wings)wing.plan=decorateCastleWingPlan(wing.plan);
  plans.set(key,site);
 }
 return plans.get(key);
}
export function castleSiteWingManifest(variant=0,wingIndex=0,seed=CASTLE_SITE_SEEDS[variant]) {
 const key=variant+':'+seed+':'+wingIndex;
 if(!manifests.has(key)) {
  const wing=castleSiteProgram(variant,seed).wings[wingIndex];
  if(!wing)throw new Error('Invalid castle wing index '+wingIndex);
  manifests.set(key,compilePlan(wing.plan));
 }
 return manifests.get(key);
}
