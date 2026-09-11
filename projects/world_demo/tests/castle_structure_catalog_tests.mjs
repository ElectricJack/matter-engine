import assert from 'node:assert/strict';
import fs from 'node:fs';
await import('./castle_shared_lib_hooks.mjs');
const C=await import('../shared-lib/castle_structure_catalog.js');
const S=await import('../shared-lib/castle_structure.js');
const Site=await import('../shared-lib/castle_site_catalog.js');
const W=await import('../shared-lib/castle_world.js');

// Independent replay: apply a stack of point functions from right to left,
// instead of the catalogue's matrix multiplication/signature implementation.
class Replay {
 constructor(matrix) { this.frames=[];this.stack=[];this.draws=[];this.material=null;this.children=[]; if(matrix)this.applyMatrix(matrix); }
 pushMatrix() { this.stack.push(this.frames.slice()); }
 popMatrix() { this.frames=this.stack.pop();assert.ok(this.frames); }
 translate(x,y,z) { this.frames.push(p=>[p[0]+x,p[1]+y,p[2]+z]); }
 rotateX(r) { this.frames.push(([x,y,z])=>[x,Math.cos(r)*y-Math.sin(r)*z,Math.sin(r)*y+Math.cos(r)*z]); }
 rotateY(r) { this.frames.push(([x,y,z])=>[Math.cos(r)*x+Math.sin(r)*z,y,-Math.sin(r)*x+Math.cos(r)*z]); }
 rotateZ(r) { this.frames.push(([x,y,z])=>[Math.cos(r)*x-Math.sin(r)*y,Math.sin(r)*x+Math.cos(r)*y,z]); }
 applyMatrix(m) { this.frames.push(p=>[0,1,2].map(i=>m[4*i]*p[0]+m[4*i+1]*p[1]+m[4*i+2]*p[2]+m[4*i+3])); }
 point(p) { return this.frames.reduceRight((v,f)=>f(v),p); }
 fill(m) { this.material=m; }
 box(center,half) {
  const vertices=[];
  for(const x of [-1,1])for(const y of [-1,1])for(const z of [-1,1])
   vertices.push(this.point(center.map((n,i)=>n+[x,y,z][i]*half[i])));
  this.draws.push(['box',this.material,vertices]);
 }
 cylinder(a,b,r) { this.draws.push(['cylinder',this.material,this.point(a),this.point(b),r]); }
 beginShape(s) { this.draws.push(['begin',this.material,s]); }
 vertex(x,y,z) { this.draws.push(['vertex',this.point([x,y,z])]); }
 endShape() { this.draws.push(['end']); }
 placeChild(module,params) { this.children.push({module,params,origin:this.point([0,0,0]),axes:[[1,0,0],[0,1,0],[0,0,1]].map(p=>this.point(p))}); }
}
function partClass(file,name,dependencies) {
 const source=fs.readFileSync(new URL('../objects/'+file,import.meta.url),'utf8').replace(/^import .*;\s*$/gm,'');
 return Function(...Object.keys(dependencies),source+'\nreturn '+name)(...Object.values(dependencies));
}
const Structure=partClass('CastleWingStructure.js','CastleWingStructure',{
 Part:Replay,castleSiteWingManifest:Site.castleSiteWingManifest,
 structureChildVariants:S.structureChildVariants,emitStructure:S.emitStructure,
});
const Assembly=partClass('CastleWingStructureAssembly.js','CastleWingStructureAssembly',{
 Part:Replay,castleMaterialsFromParams:W.castleMaterialsFromParams,CASTLE_PART_DEFAULTS:W.CASTLE_PART_DEFAULTS,
 castleWingStructurePlacements:C.castleWingStructurePlacements,
});
function near(actual,expected,label='emitted geometry') {
 if(typeof actual==='number') { assert.equal(typeof expected,'number');assert.ok(Math.abs(actual-expected)<2e-9,`${label}: ${actual} != ${expected}`); }
 else if(Array.isArray(actual)) { assert.equal(actual.length,expected.length,label);actual.forEach((v,i)=>near(v,expected[i],label)); }
 else assert.equal(actual,expected,label);
}
function replay(recipe) {
 const part=new Structure(recipe.transform);
 part.build({...Structure.params,...recipe.params});
 assert.equal(part.stack.length,0);
 assert.equal(part.children.length,0,'mesh lookup has no flattened primitive children');
 return part.draws;
}
const materials=S.structureMaterialParams(W.castleMaterialsFromParams(W.CASTLE_PART_DEFAULTS));
const options={materials,detail:1};
const catalogue=C.castleStructureCatalogue(options);
assert.equal(catalogue.records,110);
assert.equal(catalogue.unique,40);
assert.strictEqual(catalogue,C.castleStructureCatalogue({...options,siteVariant:2,siteSeed:Site.CASTLE_SITE_SEEDS[2]}),'all default sites share one cached catalogue');
const oldKeys=new Set(),newKeys=new Set();
let comparisons=0,childComparisons=0,wings=0;
function checkWing(siteVariant,wingIndex,siteSeed,extra={}) {
 const opts={...options,...extra,siteVariant,wingIndex,siteSeed};
 const manifest=Site.castleSiteWingManifest(siteVariant,wingIndex,siteSeed);
 const originals=S.structurePlacements(manifest,{...opts,module:'CastleWingStructure'});
 const replacements=C.castleWingStructurePlacements(opts);
 assert.equal(replacements.length,originals.length);
 for(let i=0;i<originals.length;i++) {
  const original=originals[i],replacement=replacements[i];
  assert.deepEqual(replacement.transform,original.transform,'original placement matrix retained exactly');
  assert.equal(replacement.module,original.module);
  if(original.module!=='CastleWingStructure') { assert.deepEqual(replacement,original,'primitive children untouched');childComparisons++;continue; }
  const source={...original,params:{...original.params,siteVariant,wingIndex,siteSeed}};
  near(replay(replacement),replay(source),`${siteVariant}/${wingIndex}/${source.params.recordId}`);
  assert.equal(S.structureChildVariants(Site.castleSiteWingManifest(replacement.params.siteVariant,replacement.params.wingIndex,replacement.params.siteSeed),replacement.params).length,0);
  comparisons++;
  if(!Object.keys(extra).length&&siteSeed===Site.CASTLE_SITE_SEEDS[siteVariant]) {
   oldKeys.add(JSON.stringify(source.params));newKeys.add(JSON.stringify(replacement.params));
  }
 }
 return replacements;
}
for(let variant=0;variant<3;variant++) {
 const site=Site.castleSiteProgram(variant);
 for(let wing=0;wing<site.wings.length;wing++) { checkWing(variant,wing,Site.CASTLE_SITE_SEEDS[variant]);wings++; }
}
assert.equal(wings,15);
assert.equal(oldKeys.size,110);assert.equal(newKeys.size,40);

// Custom manifests must use their actual seeds, while deterministic default
// representatives can still be reused when the emitted mesh really is equal.
const customSeed=73157;
const custom=C.castleStructureCatalogue({...options,siteVariant:1,siteSeed:customSeed});
assert.notStrictEqual(custom,catalogue);
assert.strictEqual(custom,C.castleStructureCatalogue({...options,siteVariant:1,siteSeed:customSeed}));
for(let wing=0;wing<Site.castleSiteProgram(1,customSeed).wings.length;wing++)checkWing(1,wing,customSeed);

// Recipe configuration boundaries may not alias, even when a current emitter
// happens to draw the same mesh at two detail values.
assert.notStrictEqual(C.castleStructureCatalogue({...options,detail:2}),catalogue);
const first=C.castleWingStructurePlacements({...options,siteVariant:0,wingIndex:0}).find(p=>p.module==='CastleWingStructure');
const sourceManifest=Site.castleSiteWingManifest(first.params.siteVariant,first.params.wingIndex,first.params.siteSeed);
const signature=C.structureMeshSignature(sourceManifest,first.params);
assert.notEqual(C.structureMeshSignature(sourceManifest,{...first.params,detail:2}),signature);
assert.notEqual(C.structureMeshSignature(sourceManifest,{...first.params,matOak:999}),signature);
assert.throws(()=>C.structureMeshSignature(sourceManifest,{...first.params,layer:2}),/mesh layer/);
assert.throws(()=>C.castleStructureCatalogue({siteVariant:99}),/Invalid castle site/);
assert.throws(()=>C.castleStructureCatalogue({siteSeed:NaN}),/Invalid castle site seed/);
checkWing(0,0,Site.CASTLE_SITE_SEEDS[0],{materials:{...materials,matSlate:107,matIron:109},detail:1.5,stairStyle:2,
 transform:[0,0,1,13,0,1,0,7,-1,0,0,-5,0,0,0,1]});

// Execute the actual modified wrapper's requires/build, checking declaration
// keys and every emitted placement, not just the shared helper in isolation.
const assemblyParams={...Assembly.params,siteVariant:2,wingIndex:1,siteSeed:Site.CASTLE_SITE_SEEDS[2]};
const expected=C.castleWingStructurePlacements({...assemblyParams,materials:W.castleMaterialsFromParams(assemblyParams)});
const requirements=Assembly.requires(assemblyParams);
const required=new Set(requirements.map(r=>r.module+JSON.stringify(r.params)));
const assembly=new Assembly();assembly.build(assemblyParams);
assert.equal(assembly.children.length,expected.length);
for(let i=0;i<expected.length;i++) {
 const child=assembly.children[i],placement=expected[i];
 assert.equal(child.module,placement.module);assert.deepEqual(child.params,placement.params);
 assert.ok(required.has(child.module+JSON.stringify(child.params)),'every placed canonical key declared');
 const frame=new Replay(placement.transform);
 near(child.origin,frame.point([0,0,0]));
 near(child.axes,[[1,0,0],[0,1,0],[0,0,1]].map(p=>frame.point(p)));
}
console.log(`castle structure catalogue: PASS — ${wings} default wings, 110 → 40 mesh keys; ${comparisons} independent world-space mesh replays, ${childComparisons} unchanged primitive placements; custom seeds, materials, detail, style, transforms and actual wrapper checked`);
