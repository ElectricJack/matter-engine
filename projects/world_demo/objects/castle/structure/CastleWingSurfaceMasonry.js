import { castleSiteWingManifest } from 'shared-lib/castle_site_catalog';
import { CASTLE_PART_DEFAULTS } from 'shared-lib/castle_world';
import { emitSiteSurfaceMasonry } from 'shared-lib/castle_site_surface_masonry';

class CastleWingSurfaceMasonry extends Part {
  static noImpostor = true;
  static lodBudgets = [1];
  static params = { ...CASTLE_PART_DEFAULTS, siteVariant: 0, wingIndex: 0, siteSeed: 9411, detail: 1, stoneMaterial: -1 };
  static requires() { return []; }
  build(p) { emitSiteSurfaceMasonry(this, castleSiteWingManifest(p.siteVariant,p.wingIndex,p.siteSeed), p); }
}
