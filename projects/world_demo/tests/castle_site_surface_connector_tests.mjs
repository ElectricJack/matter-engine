import assert from 'node:assert/strict';
import fs from 'node:fs';
await import('./castle_shared_lib_hooks.mjs');
const { castleSiteProgram,CASTLE_SITE_NAMES }=await import('../shared-lib/castle_site_catalog.js');
const { compileSite }=await import('../shared-lib/castle_site.js');
const { emitSurfaceConnector,emitConnectorMesh,connectorSolidVolumes,connectorRoofClearance,connectorCollisionEntities }=await import('../shared-lib/castle_connector_kit.js');
globalThis.SHAPE={triangles:0,polygon:3};
const params={stoneMaterial:121,floorMaterial:122,tileMaterial:123,timberMaterial:124,mortarMaterial:125};
const records=JSON.parse(fs.readFileSync(new URL('./fixtures/castle_connector_records.json',import.meta.url),'utf8')).records;
for(let i=0;i<CASTLE_SITE_NAMES.length;i++)records.push(...compileSite(castleSiteProgram(i)).connectors);
const noop=()=>{};
const legacyPart={fill:noop,beginShape:noop,endShape:noop,vertex:noop,extrude:noop};
const dot=(a,b)=>a.reduce((s,v,i)=>s+v*b[i],0),sub=(a,b)=>a.map((v,i)=>v-b[i]);
const key=p=>p.map(v=>Math.round(v*1e7)).join(',');
let triangles=0,floorPieces=0,formerlyStock=0,rafters=0;
for(const record of records) {
  const before=JSON.stringify(record),collision=connectorCollisionEntities(record);
  let emitted=0,shapeCount=0,currentMaterial,pending=[];const materials=new Set(),actualTriangles=new Map();
  const triangleKey=(m,t)=>JSON.stringify([m,...t.flatMap(p=>p.slice(0,3))]);
  const addTriangles=()=>{for(let i=0;i<pending.length;i+=3){const key=triangleKey(currentMaterial,pending.slice(i,i+3));actualTriangles.set(key,(actualTriangles.get(key)||0)+1);}};
  const part={fill(m){materials.add(m);currentMaterial=m;},beginShape(s){assert.equal(s,0);shapeCount++;pending=[];},endShape:addTriangles,
    vertex(){assert.fail('surface connector must explicitly attribute every triangle');},
    surfaceVertex(...v){assert.equal(v.length,8);assert.ok(v.every(Number.isFinite));assert.ok(Math.abs(Math.hypot(...v.slice(3,6))-1)<1e-8);pending.push(v);emitted++;}};
  const result=emitSurfaceConnector(part,record,params),legacy=emitConnectorMesh(legacyPart,record,params);
  assert.equal(shapeCount,materials.size,'one batch per material');
  assert.equal(result.materialBatches,shapeCount);
  let legacyMode,legacyMaterial,legacyPoints=[];
  emitConnectorMesh({fill(m){legacyMaterial=m;},beginShape(mode){legacyMode=mode;legacyPoints=[];},vertex(...v){legacyPoints.push(v);},extrude:noop,endShape(){
    if(legacyMode!==SHAPE.triangles)return;
    for(let i=0;i<legacyPoints.length;i+=3){
      const key=triangleKey(legacyMaterial,legacyPoints.slice(i,i+3)),count=actualTriangles.get(key)||0;
      assert.ok(count>0,'exact legacy roof position/material/winding must remain in emitted stream');actualTriangles.set(key,count-1);
    }
  }},record,params);
  assert.equal(JSON.stringify(record),before);
  assert.deepEqual(connectorCollisionEntities(record),collision,'collisions unchanged');
  assert.deepEqual(result.solidVolumes,connectorSolidVolumes(record),'finished geometry follows complete collision solids');
  assert.deepEqual(result.floorPieces,legacy.floorPieces,'all floor pieces included');
  for(const field of ['roofFacets','roofUndersides','roofFascias','roofTiles'])assert.deepEqual(result[field],legacy[field],field+' retained');
  const clear=connectorRoofClearance(record);
  assert.deepEqual(result.rafters,clear.map(c=>c.segment));
  assert.ok(clear.every(c=>c.clearance>=-1e-6),'direct rafters retain promised headroom');
  for(const shell of result.shells) {
    const edges=new Map();
    const points=shell.faces.flatMap(f=>f.positions),center=[0,1,2].map(i=>points.reduce((s,p)=>s+p[i],0)/points.length);
    for(const f of shell.faces) {
      assert.ok(dot(sub(center,f.positions[0]),f.normal)<-1e-8,'outward convex normals');
      for(let i=0;i<f.positions.length;i++) {
        const k=[key(f.positions[i]),key(f.positions[(i+1)%f.positions.length])].sort().join('|');
        edges.set(k,(edges.get(k)||0)+1);
      }
    }
    assert.ok([...edges.values()].every(v=>v===2),'closed '+shell.id);
  }
  for(const solid of result.solidVolumes) {
    const shell=result.shells.find(s=>s.id===solid.id);
    assert.equal(shell.material,solid.kind==='wall'?121:122);
    const expected=new Set(solid.points.reduce((a,_,i)=>{if(i%3===0)a.push(key(solid.points.slice(i,i+3)));return a;},[]));
    assert.deepEqual(new Set(shell.faces.flatMap(f=>f.positions.map(key))),expected,'finished footprint exactly matches collision hull');
  }
  assert.ok(!materials.has(params.mortarMaterial),'finished walls use stone material');
  assert.equal(result.children,0);
  triangles+=emitted/3;floorPieces+=result.floorPieces.length;formerlyStock+=result.floorPieces.length-legacy.inlineFloorPieces.length;rafters+=result.rafters.length;
}
assert.ok(formerlyStock>0,'exercise rectangular floors previously owned by children');
console.log('castle_site_surface_connector_tests: PASS '+JSON.stringify({connectors:records.length,triangles,floorPieces,formerlyStock,rafters,children:0}));
