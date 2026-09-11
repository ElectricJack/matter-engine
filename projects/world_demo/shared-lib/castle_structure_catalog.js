// Reuse the existing scalar lookup Part only when its emitted local mesh is
// identical. Placement transforms and independently instanced children stay put.
import { castleSiteProgram, castleSiteWingManifest, CASTLE_SITE_NAMES, CASTLE_SITE_SEEDS } from 'shared-lib/castle_site_catalog';
import { emitStructure, structureRecipes, structurePlacements, structureMaterialParams } from 'shared-lib/castle_structure';

const identity = () => [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1];
const multiply = (a,b) => a.map((_,i) => {
 const row = Math.floor(i/4), col = i%4;
 return a[row*4]*b[col]+a[row*4+1]*b[4+col]+a[row*4+2]*b[8+col]+a[row*4+3]*b[12+col];
});
const point = (m,p) => [0,1,2].map(i => m[i*4]*p[0]+m[i*4+1]*p[1]+m[i*4+2]*p[2]+m[i*4+3]);
const linear = m => [m[0],m[1],m[2],m[4],m[5],m[6],m[8],m[9],m[10]];
const stable = value => JSON.stringify(value, (_,v) => {
 if(typeof v === 'number') {
  if(!Number.isFinite(v)) throw new Error('Nonfinite structure mesh recipe');
  return Math.round(v*1e9)/1e9;
 }
 return v;
});

// Replay the real emitter, including its anchor subtraction and framed boxes.
// Keep operation order, triangle winding, material calls, box orientation and
// cylinder transforms. A whole canonical string is the key: no hash collisions.
class MeshRecorder {
 constructor() { this.matrix=identity(); this.stack=[]; this.ops=[]; }
 pushMatrix() { this.stack.push(this.matrix.slice()); }
 popMatrix() { if(!this.stack.length) throw new Error('Structure matrix underflow'); this.matrix=this.stack.pop(); }
 translate(x,y,z) { const m=identity(); m[3]=x; m[7]=y; m[11]=z; this.matrix=multiply(this.matrix,m); }
 rotateX(r) { const c=Math.cos(r),s=Math.sin(r); this.matrix=multiply(this.matrix,[1,0,0,0,0,c,-s,0,0,s,c,0,0,0,0,1]); }
 rotateY(r) { const c=Math.cos(r),s=Math.sin(r); this.matrix=multiply(this.matrix,[c,0,s,0,0,1,0,0,-s,0,c,0,0,0,0,1]); }
 rotateZ(r) { const c=Math.cos(r),s=Math.sin(r); this.matrix=multiply(this.matrix,[c,-s,0,0,s,c,0,0,0,0,1,0,0,0,0,1]); }
 fill(material) { this.ops.push(['fill',material]); }
 box(center,half) { this.ops.push(['box',point(this.matrix,center),half,linear(this.matrix)]); }
 cylinder(a,b,r) { this.ops.push(['cylinder',point(this.matrix,a),point(this.matrix,b),r,linear(this.matrix)]); }
 beginShape(shape) { this.ops.push(['beginShape',shape]); }
 vertex(x,y,z) { this.ops.push(['vertex',...point(this.matrix,[x,y,z])]); }
 endShape() { this.ops.push(['endShape']); }
}

export function structureMeshSignature(manifest,params) {
 if(params.layer!==1) throw new Error('Structure catalogue accepts mesh layer 1 only');
 const recorder=new MeshRecorder();
 emitStructure(recorder,manifest,params);
 if(recorder.stack.length) throw new Error('Unbalanced structure mesh matrix stack');
 const materials=Object.keys(params).filter(k=>k.startsWith('mat')).sort().map(k=>[k,params[k]]);
 return JSON.stringify([params.detail,params.stairStyle,materials])+':'+stable(recorder.ops);
}

const catalogues=new Map();
const catalogueData=new WeakMap();
function normalizedOptions(options) {
 const materials=options.materials && 'matOak' in options.materials
  ? {...options.materials} : structureMaterialParams(options.materials || {});
 return {module:'CastleWingStructure', materials, detail:options.detail??1, stairStyle:options.stairStyle??0};
}
const lookupKey = p => JSON.stringify([p.siteVariant,p.siteSeed,p.wingIndex,p.recordId,p.layer]);

// Defaults always come first, regardless of requires/build visitation order.
// A custom seed extends that catalogue with just its own site's wings. It never
// accidentally resolves to a manifest generated using another site's seed.
export function castleStructureCatalogue(options={}) {
 const opts=normalizedOptions(options);
 const variant=options.siteVariant??0, seed=options.siteSeed??CASTLE_SITE_SEEDS[variant];
 if(!Number.isInteger(variant)||!CASTLE_SITE_NAMES[variant]) throw new Error('Invalid castle site variant '+variant);
 if(!Number.isFinite(seed)) throw new Error('Invalid castle site seed '+seed);
 const custom=seed!==CASTLE_SITE_SEEDS[variant];
 const key=JSON.stringify([Object.entries(opts.materials).sort(([a],[b])=>a<b?-1:a>b?1:0),opts.detail,opts.stairStyle,custom?[variant,seed]:null]);
 if(catalogues.has(key)) return catalogues.get(key);
 const base=custom?catalogueData.get(castleStructureCatalogue({...opts,siteVariant:variant,siteSeed:CASTLE_SITE_SEEDS[variant]})):null;
 const representatives=new Map(base?.representatives),recipes=new Map(base?.recipes);
 const sites=custom?[[variant,seed]]:CASTLE_SITE_NAMES.map((_,siteVariant)=>[siteVariant,CASTLE_SITE_SEEDS[siteVariant]]);
 let records=base?.records??0;
 for(const [siteVariant,siteSeed] of sites) {
  const site=castleSiteProgram(siteVariant,siteSeed);
  for(let wingIndex=0;wingIndex<site.wings.length;wingIndex++) {
   const manifest=castleSiteWingManifest(siteVariant,wingIndex,siteSeed);
   for(const recipe of structureRecipes(manifest,opts)) {
    if(recipe.params.layer!==1) continue;
    const params={...recipe.params,siteVariant,wingIndex,siteSeed};
    const signature=structureMeshSignature(manifest,params);
    if(!representatives.has(signature)) representatives.set(signature,Object.freeze(params));
    recipes.set(lookupKey(params),representatives.get(signature));
    records++;
   }
  }
 }
 const result=Object.freeze({records,unique:representatives.size,resolve(params) {
  const representative=recipes.get(lookupKey(params));
  if(!representative) throw new Error('Missing structure catalogue record '+lookupKey(params));
  return representative;
 }});
 catalogueData.set(result,{representatives,recipes,records});
 catalogues.set(key,result);
 return result;
}

export function castleWingStructurePlacements(options={}) {
 const siteVariant=options.siteVariant??0,wingIndex=options.wingIndex??0;
 const siteSeed=options.siteSeed??CASTLE_SITE_SEEDS[siteVariant];
 const opts=normalizedOptions(options);
 const catalogue=castleStructureCatalogue({...opts,siteVariant,siteSeed});
 const manifest=castleSiteWingManifest(siteVariant,wingIndex,siteSeed);
 return structurePlacements(manifest,{...options,...opts}).map(placement=> {
  if(placement.module!=='CastleWingStructure'||placement.params.layer!==1) return placement;
  const params=catalogue.resolve({...placement.params,siteVariant,wingIndex,siteSeed});
  return {...placement,params:{...params}};
 });
}
