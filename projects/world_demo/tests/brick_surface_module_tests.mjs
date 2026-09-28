import assert from 'node:assert/strict';
import { brickSurfaceModule, brickModuleAddress, brickWallLayout } from '../shared-lib/brick_wall_layout.js';
await import('./castle_shared_lib_hooks.mjs');
const { brickWallGeometryPlan } = await import('../shared-lib/brick_wall_geometry.js');
const { clayBrickWallSurface, clayBrickWallReceiver } = await import('../shared-lib/clay_brick_wall_surface.js');
const close = (a,b) => assert.ok(Math.abs(a-b)<1e-9,`${a} != ${b}`);
const recipe = {moduleColumns:8,moduleCourses:4,seed:123};
const module = brickSurfaceModule(recipe);
close(module.periodM[0],2.04);close(module.periodM[1],.376);
const counts=[1,2,4];
const walls=counts.map(k=>brickWallLayout({...recipe,columns:8*k,courses:4*k}));
for(const [i,w] of walls.entries()) {
  const k=counts[i];
  assert.equal(w.surfaceModule.layoutKey,module.layoutKey,'wall dimensions are outside module identity');
  close(w.widthM+.010,k*module.periodM[0]);close(w.heightM+.010,k*module.periodM[1]);
  assert.equal(clayBrickWallReceiver({...recipe,columns:8*k,courses:4*k}).faces.length,6,
    'periodic appearance retains the six-face receiver');
  for(const p of w.placements) {
    const ext=p.bounds.max.map((v,j)=>Math.round((v-p.bounds.min[j])*1e6)).sort((a,b)=>a-b);
    assert.deepEqual(ext,[84000,117500,245000],'repeat counts never scale or crop bricks');
    assert.deepEqual(p.surfaceCell,brickModuleAddress(module,p.course,p.column,p.wythe,p.role));
  }
  // Compare translated, real interior bricks against the first period; this
  // tests geometry and appearance together, rather than only modulo arithmetic.
  for(const p of w.placements.filter(p=>p.role==='stretcher')) {
    const base=w.placements.find(q=>q.role===p.role && q.wythe===p.wythe &&
      q.course===p.course%module.courses && q.column===p.column%module.columns);
    if(!base) continue; // Exposed ends replace a crossing stretcher with a header.
    assert.equal(p.variant,base.variant);
    assert.equal(p.appearanceSeed,base.appearanceSeed);
    close(p.matrix[3]-base.matrix[3],Math.floor(p.column/module.columns)*module.periodM[0]);
    close(p.matrix[7]-base.matrix[7],Math.floor(p.course/module.courses)*module.periodM[1]);
  }
  for(let row=1;row<w.courses;row+=2) {
    const headers=w.placements.filter(p=>p.course===row && p.role.startsWith('header'));
    assert.equal(headers.length,2,'each odd course has exactly two end treatments');
    close(headers[0].bounds.min[0],0);close(headers[1].bounds.max[0],w.widthM);
  }
}
// A repeat cut through an even course contains exactly one joint; the odd
// course has one whole stretcher crossing it. No duplicate/missing joint or
// artificial header is inserted at internal repeat boundaries.
const wide=walls[2];
for(const cut of [module.periodM[0],2*module.periodM[0],3*module.periodM[0]]) {
  for(const row of [0,1]) {
    const run=wide.placements.filter(p=>p.course===row && p.wythe===0);
    const crossing=run.filter(p=>p.bounds.min[0]<cut-1e-9 && p.bounds.max[0]>cut+1e-9);
    assert.equal(crossing.length,row%2);
    if(!row) {
      const before=Math.max(...run.filter(p=>p.bounds.max[0]<=cut+1e-9).map(p=>p.bounds.max[0]));
      const after=Math.min(...run.filter(p=>p.bounds.min[0]>=cut-1e-9).map(p=>p.bounds.min[0]));
      close(after-before,.010);
    }
  }
}
const shifted=brickSurfaceModule({...recipe,phaseColumns:-3,phaseCourses:-2});
assert.equal(shifted.layoutKey,module.layoutKey,'phase changes mapping, not module content identity');
for(const row of [-17,-4,-1,0,1,4,17]) for(const col of [-19,-8,-1,0,1,8,19]) {
  assert.deepEqual(brickModuleAddress(module,row,col),brickModuleAddress(module,row+4,col+8));
  assert.deepEqual(brickModuleAddress(shifted,row,col),brickModuleAddress(module,row-2,col-3));
}
const huge=Number.MAX_SAFE_INTEGER;
const bigMod=(n,p)=>Number((BigInt(n)%BigInt(p)+BigInt(p))%BigInt(p));
for(const sign of [-1,1]) {
  const address=brickModuleAddress(shifted,sign*huge,sign*huge);
  assert.equal(address.course,(bigMod(sign*huge,4)+2)%4);
  assert.equal(address.column,(bigMod(sign*huge,8)+5)%8);
}
const appearance=brickSurfaceModule({...recipe,columns:37,courses:9,weatherSeed:99,weathering:1});
assert.equal(appearance.layoutKey,module.layoutKey);
for(const edit of [{seed:124},{moduleColumns:16},{moduleCourses:8},{headJointM:.012,brick:{lengthM:.247}},
                   {bedJointM:.012},{brick:{heightM:.08}}])
  assert.notEqual(brickSurfaceModule({...recipe,...edit}).layoutKey,module.layoutKey);
for(const invalid of [{moduleColumns:0},{moduleCourses:3},{moduleCourses:Infinity},{phaseColumns:.5},
                      {phaseCourses:1},{phaseColumns:2**32},{seed:-1}])
  assert.throws(()=>brickSurfaceModule({...recipe,...invalid}),RangeError);
assert.throws(()=>brickWallLayout({phaseColumns:1}),RangeError);
assert.equal(brickSurfaceModule({bond:'stack',moduleCourses:3,phaseCourses:-1}).phase[1],2);

// Receiver and high-detail geometry consume identical placements. Neither
// repeat count nor phase can trigger preparation of a different brick bank.
const surfaces=counts.map(k=>clayBrickWallSurface({...recipe,columns:8*k,courses:4*k}));
for(let i=1;i<surfaces.length;++i)
  assert.equal(JSON.stringify(surfaces[i].sources),JSON.stringify(surfaces[0].sources));
const materialJSON=s=>JSON.stringify(s.periodic.modules);
for(let i=0;i<surfaces.length;++i) {
  const {modules,mappings}=surfaces[i].periodic,w=walls[i];
  assert.equal(modules.length,2,'one reusable material for each wall side');
  assert.equal(materialJSON(surfaces[i]),materialJSON(surfaces[0]),'module content ignores wall dimensions');
  for(const m of modules)assert.equal(m.placements.length,32,'material has one fixed 8 by 4 source period');
  const margin=w.brick.depthM+w.headJointM*.5;
  close(mappings[0].uRangeM[0],margin);close(mappings[0].uRangeM[1],w.widthM-margin);
  close(mappings[1].uRangeM[0],module.periodM[0]-w.widthM+margin);
  close(mappings[1].uRangeM[1],module.periodM[0]-margin);
  // Check authored source variants and physical placements against actual
  // interior bricks, on both sides and on both running-bond row parities.
  for(let side=0;side<2;++side)for(const p of w.placements.filter(p=>
      p.role==='stretcher' && p.wythe===(side===0?1:0) && p.course<4 && p.column<8)) {
    const q=modules[side].placements[p.course*8+p.column];
    assert.equal(q.source,p.variant);q.matrix.forEach((v,k)=>close(v,p.matrix[k]));
  }
}
const phaseSurface=clayBrickWallSurface({...recipe,columns:16,courses:8,phaseColumns:-3,phaseCourses:-2});
assert.equal(materialJSON(phaseSurface),materialJSON(surfaces[0]),'phase leaves module content unchanged');
close(phaseSurface.periodic.mappings[0].phase[0],5/8);
close(phaseSurface.periodic.mappings[1].phase[0],-5/8);
close(phaseSurface.periodic.mappings[0].phase[1],.5);
assert.equal(clayBrickWallSurface({...recipe,weathering:1}).periodic,undefined,
  'weathered walls retain their full composition until sparse overrides are supported');
const physical=brickWallGeometryPlan({...recipe,columns:16,courses:8});
assert.deepEqual(physical.children.map(p=>[p.matrix,p.variant]),walls[1].placements.map(p=>[p.matrix,p.variant]));
assert.deepEqual(surfaces[1].placements,walls[1].placements.map(p=>({id:p.id,source:p.variant,matrix:p.matrix})));
console.log('brick surface module: PASS (1x/2x/4x, shared layout identity, terminal joints, whole ends, negative phase, source reuse)');
