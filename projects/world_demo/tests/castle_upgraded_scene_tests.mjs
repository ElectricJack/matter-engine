// Whole-scene contract: every authored root really emits geometry, preserves
// the walkable site's collision/light layout, and requests no primitive bakes.
import assert from 'node:assert/strict';
import { createCastleHarness } from './helpers/castle_dsl_harness.mjs';
const upgraded=await createCastleHarness('CastleUpgraded');
const legacy=await createCastleHarness('CastleClusteredCourt');
assert.equal(JSON.stringify(upgraded.world.entities),JSON.stringify(legacy.world.entities));
assert.equal(JSON.stringify(upgraded.world.lights),JSON.stringify(legacy.world.lights));
const detail=upgraded.materials.filter(m=>m.spec.detailMode==='surface');
assert.equal(detail.length,1);assert.equal(detail[0].spec.detail,'CastleBrickBondDetail');
const counts={},seen=new Set(),calls={};let voxelParts=0;
for(const root of upgraded.world.roots) {
 counts[root.module]=(counts[root.module]||0)+1;
 assert.ok(!root.expand,root.module+' direct geometry must not be expanded away');
 const m=root.transform||[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1];
 for(let a=0;a<3;a++)for(let b=0;b<3;b++) {
  const dot=m[a]*m[b]+m[4+a]*m[4+b]+m[8+a]*m[8+b];
  assert.ok(Math.abs(dot-(a===b?1:0))<2e-5,root.module+' root must be rigid');
 }
 const key=root.module+JSON.stringify(root.params);
 if(seen.has(key))continue;seen.add(key);
 const part=await upgraded.build(root.module,root.params);
 assert.equal(part.requires.length,0,root.module+' must not require source meshes');
 assert.equal(part.children.length,0,root.module+' must draw its own finished geometry');
 // Furniture uses a local ellipsoid constructor for metal rosettes/flames.
 // The no-scaling contract applies to reusable Part placements, checked above;
 // all wall/structure/floor adapters additionally avoid DSL scale entirely.
 if(root.module.includes('Surface'))assert.equal(part.calls.__dsl_scale||0,0,root.module+' cannot fit stock by scaling');
 assert.ok(Object.values(part.calls).some(n=>n>0),root.module+' cannot be empty');
 if(part.calls.__dsl_beginVoxels)voxelParts++;
 for(const [name,n] of Object.entries(part.calls))calls[name]=(calls[name]||0)+n;
}
assert.equal(counts.CastleWingSurfaceMasonry,4);
assert.equal(counts.CastleWingSurfaceStructure,33);
assert.equal(counts.CastleSiteSurfaceConnector,4);
assert.ok(counts.CastleSiteSurfacePaving>0);
assert.ok(counts.CastleWindowGlazing>0&&counts.CastleAltar>0);
assert.equal(upgraded.world.lights.points.length,132);
console.log('castle_upgraded_scene_tests: PASS '+JSON.stringify({roots:upgraded.world.roots.length,uniqueParts:seen.size,entities:upgraded.world.entities.length,points:132,spots:upgraded.world.lights.spots.length,voxelParts,counts,calls}));
