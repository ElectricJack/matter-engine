// Native construction check of a complete local hall before site placement.
import { castleSiteWingManifest } from 'shared-lib/castle_site_catalog';
import { castleMaterialParams } from 'shared-lib/castle_world';
import { defineFurnishingMaterials } from 'shared-lib/castle_furnishings';
import { castleFurnishingLayout } from 'shared-lib/castle_furnishing_layout';
import { castleCollisionEntities } from 'shared-lib/castle_collision';

export function castleWingStudyDefinition() {
 const manifest=castleSiteWingManifest(0,1),materials=defineFurnishingMaterials('CastleWingStudy');
 const lookup={siteVariant:0,wingIndex:1,siteSeed:9411};
 const roots=[{module:'CastleWingMasonry',params:{...lookup,...castleMaterialParams(materials)},expand:true}];
 roots.push({module:'CastleWingStructureAssembly',params:{...lookup,...castleMaterialParams(materials)},expand:true});
 const furnishings=castleFurnishingLayout(manifest,materials);
 roots.push(...furnishings.roots);
 const entities=castleCollisionEntities(manifest,{prefix:'hall-study'});
 return {roots,entities,lights:{sun:{dir:[.42,-.78,-.46],color:[1,.92,.8]},sky:{color:[.14,.19,.27]},
  points:furnishings.points,spots:furnishings.spots},
  camera:{position:[23,16,-18],target:[7,4,4]},atmosphere:{groundAlbedo:.3}};
}
