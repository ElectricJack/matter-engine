import assert from 'node:assert/strict';
import fs from 'node:fs';
await import('./castle_shared_lib_hooks.mjs');
const { castleSiteProgram, castleSiteWingManifest, CASTLE_SITE_NAMES } = await import('../shared-lib/castle_site_catalog.js');
const { layoutSiteSurfaceMasonry, emitSiteSurfaceMasonry } = await import('../shared-lib/castle_site_surface_masonry.js');
const { wallModuleSockets, masonryOptions } = await import('../shared-lib/castle_masonry.js');
const dot=(a,b)=>a.reduce((n,v,i)=>n+v*b[i],0);
const sub=(a,b)=>a.map((v,i)=>v-b[i]);
const key=p=>p.map(x=>Math.round(x*1e7)).join(',');
const inside=(s,p)=>s.faces.every(f=>dot(sub(p,f.positions[0]),f.normal)<-1e-6);
const total={wings:0,wallModules:0,junctions:0,openings:0,arches:0,shells:0,triangles:0};
for(let variant=0;variant<CASTLE_SITE_NAMES.length;variant++) {
  const site=castleSiteProgram(variant);
  for(let wing=0;wing<site.wings.length;wing++) {
    const manifest=castleSiteWingManifest(variant,wing),before=JSON.stringify(manifest);
    const layout=layoutSiteSurfaceMasonry(manifest,{stoneMaterial:121,oak:122});
    assert.equal(JSON.stringify(manifest),before,'adapter must not mutate authoring manifest');
    const expected=manifest.wallModules.flatMap(r=>wallModuleSockets(r,manifest,masonryOptions({})));
    assert.deepEqual(layout.sockets,expected,'retain exact legacy attachment and aperture dimensions');
    for(const shell of layout.shells) {
      const edges=new Map();
      for(const face of shell.faces) {
        assert.ok(face.positions.every(p=>p.every(Number.isFinite)));
        assert.ok(Math.abs(Math.hypot(...face.normal)-1)<1e-8);
        for(const [i,p] of face.positions.entries()) {
          const q=face.positions[(i+1)%face.positions.length],a=key(p),b=key(q),e=[a,b].sort().join('|');
          assert.notEqual(a,b,'no degenerate edge');
          edges.set(e,(edges.get(e)||0)+1);
        }
      }
      assert.ok([...edges.values()].every(n=>n===2),shell.id+' must be closed');
      const centre=shell.solid[0].map((_,i)=>shell.solid.reduce((n,p)=>n+p[i],0)/shell.solid.length);
      assert.ok(inside(shell,centre),shell.id+' faces must point outwards');
      assert.equal(shell.material,shell.kind==='rail'?122:121);
    }
    for(const socket of layout.sockets) {
      // Entire promised rectangle stays empty through both mouths and centre.
      const y0=socket.origin[1],height=socket.clearTop-socket.clearBottom;
      for(const side of [-.49,0,.49])for(const across of [-.49,0,.49])for(const dy of [.01,height/2,height-.01]) {
        const p=socket.origin.map((v,i)=>v+socket.u[i]*socket.width*side+socket.w[i]*socket.thickness*across+(i===1?dy:0));
        assert.ok(!layout.shells.some(s=>s.kind!=='rail'&&inside(s,p)),socket.id+' promised aperture is blocked');
      }
      if(socket.arch) {
        const p=[...socket.origin];p[1]=y0-socket.clearBottom+socket.arch.springY+socket.arch.rise*.5;
        assert.ok(!layout.shells.some(s=>s.kind!=='rail'&&inside(s,p)),socket.id+' arch crown opening retained');
      }
    }
    assert.equal(layout.census.children,0);
    assert.ok(layout.census.triangles<manifest.wallModules.length*80,'wall geometry bounded independently of brick courses');
    total.wings++;for(const k of Object.keys(total).filter(k=>k!=='wings'))total[k]+=layout.census[k];
  }
}
const m=castleSiteWingManifest(0,0),layout=layoutSiteSurfaceMasonry(m,{stoneMaterial:123});
let vertices=0,shapes=0;const fills=new Set();
emitSiteSurfaceMasonry({fill(m){fills.add(m);},beginShape(){shapes++;},surfaceVertex(...v){assert.equal(v.length,8);vertices++;},endShape(){}},m,{stoneMaterial:123});
assert.equal(vertices,layout.census.triangles*3);assert.ok(fills.has(123));assert.equal(shapes,fills.size,'one shape per material');
const {emitSurfaceShell}=await import('../shared-lib/castle_surface_shells.js');
const expectedBuckets=new Map(),actualBuckets=new Map();
function recorder(buckets){let active;return {fill(m){if(!buckets.has(m))buckets.set(m,[]);active=buckets.get(m);},beginShape(){},endShape(){},surfaceVertex(...v){active.push(v);}};}
const reference=recorder(expectedBuckets);
for(const shell of layout.shells)emitSurfaceShell(reference,shell,{body:shell.material});
emitSiteSurfaceMasonry(recorder(actualBuckets),m,{stoneMaterial:123});
assert.deepEqual(actualBuckets,expectedBuckets,'batching preserves every attributed vertex exactly');
assert.throws(()=>layoutSiteSurfaceMasonry({...m,curves:[{}]}),/radial adapter/);
const source=fs.readFileSync(new URL('../objects/CastleWingSurfaceMasonry.js',import.meta.url),'utf8');
assert.match(source,/static requires\(\) \{ return \[\]; \}/);
console.log('castle_site_surface_masonry_tests: PASS '+JSON.stringify(total));
