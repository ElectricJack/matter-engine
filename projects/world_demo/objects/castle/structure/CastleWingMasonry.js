import { castleSiteWingManifest } from 'shared-lib/castle_site_catalog';
import { CASTLE_PART_DEFAULTS } from 'shared-lib/castle_world';
import { masonryOptions, masonryChildVariants, emitMasonry } from 'shared-lib/castle_masonry';

class CastleWingMasonry extends Part {
 static noImpostor=true;
 static lodBudgets=[1];
 static params={...CASTLE_PART_DEFAULTS,siteVariant:0,wingIndex:0,siteSeed:9411,detail:1};
 static requires(p){return masonryChildVariants(castleSiteWingManifest(p.siteVariant,p.wingIndex,p.siteSeed),masonryOptions(p));}
 build(p){emitMasonry(this,castleSiteWingManifest(p.siteVariant,p.wingIndex,p.siteSeed),masonryOptions(p));}
}
