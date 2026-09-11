import { castleSceneManifest, CASTLE_PART_DEFAULTS } from 'shared-lib/castle_world';
import { masonryOptions, masonryChildVariants, emitMasonry } from 'shared-lib/castle_masonry';

class CastleMasonryAssembly extends Part {
 static noImpostor=true;
 static lodBudgets=[1];
 static params={...CASTLE_PART_DEFAULTS,detail:1};
 static requires(p){return masonryChildVariants(castleSceneManifest(p.variant,p.seed),masonryOptions(p));}
 build(p){emitMasonry(this,castleSceneManifest(p.variant,p.seed),masonryOptions(p));}
}
