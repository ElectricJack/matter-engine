import assert from 'node:assert/strict';
import { brickWallLayout, MODULAR_CLAY_BRICK } from '../shared-lib/brick_wall_layout.js';
await import('./castle_shared_lib_hooks.mjs');
const { brickWallGeometryPlan, emitBrickWallGeometry } = await import('../shared-lib/brick_wall_geometry.js');
const close = (a,b) => assert.ok(Math.abs(a-b)<1e-9, `${a} != ${b}`);
const volumeOverlap = (a,b) => [0,1,2].every(i => Math.min(a.max[i],b.max[i])-Math.max(a.min[i],b.min[i])>1e-9);
let wallCases=0, brickCases=0;
for (const bond of ['stack','running-headers']) for (const columns of [1,2,3,5,9]) for (const courses of [1,2,3,7]) {
  const w=brickWallLayout({bond,columns,courses});
  ++wallCases; brickCases+=w.placements.length;
  assert.equal(w.placements.length,columns*courses*(bond==='stack'?1:2));
  assert.equal(new Set(w.placements.map(b=>b.id)).size,w.placements.length);
  close(w.widthM,columns*.245+(columns-1)*.010);
  close(w.heightM,courses*.084+(courses-1)*.010);
  for (let i=0;i<w.placements.length;++i) {
    const a=w.placements[i], m=a.matrix, bounds=a.bounds;
    const size=bounds.max.map((v,k)=>v-bounds.min[k]);
    assert.deepEqual(size.map(v=>Math.round(v*1e6)).sort((a,b)=>a-b),[84000,117500,245000]);
    for (let k=0;k<3;++k) {
      assert.ok(bounds.min[k]>=-1e-9 && bounds.max[k]<=w.bounds.max[k]+1e-9);
      close(Math.hypot(...m.slice(4*k,4*k+3)),1);
    }
    for (const x of [-.245/2,.245/2]) for (const y of [0,.084]) for (const z of [-.1175/2,.1175/2]) {
      const point=[0,1,2].map(k=>m[k*4]*x+m[k*4+1]*y+m[k*4+2]*z+m[k*4+3]);
      assert.ok(point.every((v,k)=>Math.abs(v-bounds.min[k])<1e-9 || Math.abs(v-bounds.max[k])<1e-9));
    }
    for (let j=0;j<i;++j) assert.ok(!volumeOverlap(bounds,w.placements[j].bounds),'bricks cannot interpenetrate');
    for (const g of w.mortar) assert.ok(!volumeOverlap(bounds,g),'mortar occupies joints, not brick volume');
  }
  for (let row=0;row<courses;++row) {
    const bricks=w.placements.filter(p=>p.course===row);
    close(Math.min(...bricks.map(p=>p.bounds.min[0])),0);
    close(Math.max(...bricks.map(p=>p.bounds.max[0])),w.widthM);
    close(Math.min(...bricks.map(p=>p.bounds.min[2])),0);
    close(Math.max(...bricks.map(p=>p.bounds.max[2])),w.thicknessM);
    if (bond==='running-headers' && (row&1)) {
      const headers=bricks.filter(p=>p.role.startsWith('header'));
      assert.equal(headers.length,2);
      headers.forEach(p=>close(p.bounds.max[2]-p.bounds.min[2],w.thicknessM));
    }
  }
  for (const g of w.mortar) assert.ok(g.min.every((v,i)=>v>=0 && g.max[i]<=w.bounds.max[i]+1e-9));
  const larger=brickWallLayout({bond,columns:columns+2,courses:courses+2});
  for (const b of w.placements.filter(p=>p.role!=='header-right')) {
    const match=larger.placements.find(p=>p.id===b.id);
    assert.deepEqual(match,b,'resize preserves anchored brick, transform and variant');
  }
}
for (const fit of ['inside','outside','nearest']) for (const widthM of [.245,.254,1,1.02,2.5]) {
  const w=brickWallLayout({widthM,heightM:.8,fit});
  if (fit==='inside') assert.ok(w.widthM<=widthM+1e-9 && w.heightM<=.8+1e-9);
  if (fit==='outside') assert.ok(w.widthM>=widthM-1e-9 && w.heightM>=.8-1e-9);
  if (fit==='nearest') assert.ok(Math.abs(w.widthM-widthM)<=.255/2+1e-9 && Math.abs(w.heightM-.8)<=.094/2+1e-9);
  close(w.adjustmentM.width,w.widthM-widthM);
}
const snapped=brickWallLayout({widthM:1,heightM:.8});
assert.equal(snapped.columns,4); assert.equal(snapped.courses,9);
close(snapped.widthM,1.01); close(snapped.heightM,.836);
for (const p of [
  {columns:0}, {courses:1.5}, {columns:NaN}, {widthM:Infinity}, {widthM:.1,fit:'inside'},
  {columns:2,widthM:1}, {thicknessM:.12}, {brick:{depthM:.112}}, {headJointM:0},
  {columns:4096,courses:4096}, {mortarRecessM:.1}, {seed:-1}, {bond:'cropped'},
  {fit:'stretch'}, {openings:[{}]}, {corner:{}}, {maxBricks:0},
]) assert.throws(()=>brickWallLayout(p),RangeError);
assert.equal(brickWallLayout({bond:'stack',brick:{depthM:.112}}).thicknessM,.112);
assert.deepEqual(MODULAR_CLAY_BRICK,{lengthM:.245,heightM:.084,depthM:.1175});
// Orthogonal metric face bases preserve texture scale on headers, top and ends.
for (const f of brickWallLayout().sourceFaces) {
  close(Math.hypot(...f.u),1); close(Math.hypot(...f.v),1);
  assert.deepEqual([f.u[1]*f.v[2]-f.u[2]*f.v[1], f.u[2]*f.v[0]-f.u[0]*f.v[2],
    f.u[0]*f.v[1]-f.u[1]*f.v[0]].map(v=>v||0),f.n);
}
console.log(`brick wall layout: PASS (${wallCases} wall sizes/bonds, ${brickCases} whole bricks; fit, resize, mortar and metric frames)`);
const plan=brickWallGeometryPlan({columns:5,courses:6});
const emitted=[],matrices=[];
emitBrickWallGeometry({fill(){},beginShape(){},surfaceVertex(){},endShape(){},pushMatrix(){},popMatrix(){},
  applyMatrix(m){matrices.push(m);},placeChild(module,params,options){emitted.push({module,params,options});}},plan);
assert.equal(emitted.length,plan.layout.placements.length);
assert.deepEqual(matrices,plan.layout.placements.map(p=>p.matrix));
assert.ok(emitted.every(p=>plan.requires.some(r=>r.module===p.module && JSON.stringify(r.params)===JSON.stringify(p.params))));
assert.ok(emitted.every(p=>p.params.length===.245 && p.params.depth===.1175 && p.options.instanced));
const larger=brickWallGeometryPlan({columns:8,courses:9});
assert.deepEqual(plan.requires,larger.requires,'wall resize reuses identical source assets');
assert.ok(plan.mortar.faces.every(f=>f.uv.every(uv=>uv.every(Number.isFinite))));
assert.throws(()=>brickWallGeometryPlan({brickMaterials:[8]}),TypeError);
const scalarMaterials=brickWallGeometryPlan({...Object.fromEntries(Array.from({length:8},(_,i)=>['matBrick'+i,100+i])),mortarMaterial:110});
assert.ok(scalarMaterials.children.every(p=>p.params.material===100+p.variant),'native scalar material handles reach brick sources');
console.log('brick wall geometry: PASS (shared layout, rigid child placement, bounded source reuse, metric mortar UVs)');
