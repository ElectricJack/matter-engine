// Stable scalar lookups shared by world roots and independently baked Parts.
import { decorateCastleWingPlan } from 'shared-lib/castle_wing_interiors';
import { compilePlan } from 'shared-lib/castle_plan';
import { castleSitePlan, CASTLE_SITE_NAMES, CASTLE_SITE_SEEDS } from 'shared-lib/castle_site_programs';

export { CASTLE_SITE_NAMES, CASTLE_SITE_SEEDS };
const plans = new Map(), manifests = new Map(), selectedPlans = new Map();
const clone = value => JSON.parse(JSON.stringify(value));

function checkedSite(variant, seed) {
  if (!Number.isInteger(variant) || !CASTLE_SITE_NAMES[variant])
    throw new Error('Invalid castle site variant ' + variant);
  return castleSitePlan(CASTLE_SITE_NAMES[variant], seed);
}

export function castleSiteProgram(variant = 0, seed = CASTLE_SITE_SEEDS[variant]) {
  if (!Number.isInteger(variant) || !CASTLE_SITE_NAMES[variant])
    throw new Error('Invalid castle site variant ' + variant);
  const key = variant + ':' + seed;
  if (!plans.has(key)) {
    const site = checkedSite(variant, seed);
    site.wings.forEach((wing, index) => {
      const selected = selectedPlans.get(key + ':' + index);
      // The full-site API returns mutable authoring data. Never expose the
      // private selected-wing plan through an alias to that returned object.
      wing.plan = selected ? clone(selected) : decorateCastleWingPlan(wing.plan);
    });
    plans.set(key, site);
  }
  return plans.get(key);
}

export function castleSiteWingManifest(variant = 0, wingIndex = 0, seed = CASTLE_SITE_SEEDS[variant]) {
  const siteKey = variant + ':' + seed;
  const key = siteKey + ':' + wingIndex;
  if (!manifests.has(key)) {
    const complete = plans.get(siteKey);
    const site = complete || checkedSite(variant, seed);
    const wing = site.wings[wingIndex];
    if (!wing) throw new Error('Invalid castle wing index ' + wingIndex);
    let plan = wing.plan;
    if (!complete) {
      // A Part's requires()/build() needs only this wing. Decorating every
      // unrelated wing here repeated seconds of native QuickJS work per Part.
      // decorateCastleWingPlan clones its input; raw site plans stay untouched.
      if (!selectedPlans.has(key)) selectedPlans.set(key, decorateCastleWingPlan(plan));
      plan = selectedPlans.get(key);
    }
    manifests.set(key, compilePlan(plan));
  }
  return manifests.get(key);
}
