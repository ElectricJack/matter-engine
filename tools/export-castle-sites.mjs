// Export the exact manifests consumed by the native scene and walkthrough tool.
import fs from 'node:fs';
import path from 'node:path';
await import('../projects/world_demo/tests/castle_shared_lib_hooks.mjs');
const {castleSceneSite}=await import('../projects/world_demo/shared-lib/castle_site_world.js');
const {siteToJSON,siteToSVG}=await import('../projects/world_demo/shared-lib/castle_site.js');
const {CASTLE_SITE_NAMES}=await import('../projects/world_demo/shared-lib/castle_site_catalog.js');
const output=path.resolve(process.argv[2]||'build/qa/castle-sites');
fs.mkdirSync(output,{recursive:true});
for(let variant=0;variant<CASTLE_SITE_NAMES.length;variant++) {
 const name=CASTLE_SITE_NAMES[variant],manifest=castleSceneSite(variant);
 fs.writeFileSync(path.join(output,name+'.json'),siteToJSON(manifest));
 fs.writeFileSync(path.join(output,name+'.svg'),siteToSVG(manifest,{showRoutes:true}));
 const levels=[...new Set(manifest.wings.flatMap(wing=>wing.manifest.levels.map(level=>level.id)))];
 for(const levelId of levels) {
  const filename=name+'-'+encodeURIComponent(levelId)+'.svg';
  fs.writeFileSync(path.join(output,filename),siteToSVG(manifest,{levelId,showRoutes:true}));
 }
 console.log(JSON.stringify({name,wings:manifest.wings.length,connectors:manifest.connectors.length,
  courtyards:manifest.courtyards.length,rooms:manifest.roomGraph.reachableRoomIds.length,
  routes:manifest.walkRoutes.length,levels}));
}
