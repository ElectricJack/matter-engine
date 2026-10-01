import { emitFixtureGlow, FIXTURE_GLOW_DEFAULTS } from 'shared-lib/castle_furnishings';

// Cosmetic candle-flame glow for a lit fixture (fixture: 0 sconce,
// 1 chandelier, 2 altar). It calls rayTraced(false): the paired analytic
// lights own the source power, so this emissive proxy never also transports
// it through ray-traced GI. It shares its fixture's root transform.
class CastleFixtureGlow extends Part {
  static lodBudgets = [1];
  static noImpostor = true;
  static params = { ...FIXTURE_GLOW_DEFAULTS };
  build(p) { emitFixtureGlow(this, p); }
}
