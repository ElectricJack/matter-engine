// Reproduce the provenance/population audit from immutable Git objects.
// This reads Git and evaluates recording JS fixtures; it launches no editor.
import assert from 'node:assert/strict';
import {execFileSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import {fileURLToPath} from 'node:url';

const repo = fileURLToPath(new URL('../../../../', import.meta.url));
const git = (...args) => execFileSync('git', args, {cwd:repo, encoding:'utf8'}).trim();
const source = (commit, path) => execFileSync('git', ['show', `${commit}:${path}`], {cwd:repo, encoding:'utf8'});
const sha256 = text => createHash('sha256').update(text).digest('hex');
const load = text => import('data:text/javascript;base64,' + Buffer.from(text).toString('base64'));
const main = '8aab2e54bbbffbb0f6044ec3bda36974e30b864b';
const vg = '6c70efe2e64dee7ff893ac892c38c2c994f03837';
const base = 'd753087d88417ab5b77cf77369c1e38e22cee6b9';
const relocation = git('rev-parse', '912ad0fb3');
const content = git('rev-parse', '4228d4c9d');
const stale = git('rev-parse', 'd006f20e9');
assert.equal(git('merge-base', main, vg), base);
const ancestor = (a,b) => git('merge-base', a,b) === a;
const path = 'projects/world_demo/shared-lib/mountain_geometry_site.js';
const siteSource = source(vg,path);
assert.equal(siteSource, source(content,path));
const {mountainGeometrySamples, mountainGeometryCatalog} = await load(siteSource);
// A sentinel material ID makes parameter identity reproducible independently
// of the runtime material registry. Production supplies Mountain.GeometryRock.
const samples = mountainGeometrySamples(7);
const focal = samples.slice(0,3);
assert.equal(samples.length,1283);
assert.equal(mountainGeometryCatalog(7).length,19);
assert.deepEqual(focal.map(s=>[s.x,s.z,s.params.seed,s.params.shape,s.params.size]),
  [[416,1456,0,0,6],[425,1458,1,2,4],[422,1448,2,1,7]]);
assert.ok(samples.every(s=>s.params.resolution===128));
const small = samples.slice(3).filter(s=>s.params.seed>=108);
const large = samples.slice(3).filter(s=>s.params.seed<108);
assert.equal(small.length,1024);
assert.equal(large.length,256);
const bounds = rows => ({
  min:[Math.min(...rows.map(s=>s.x)),Math.min(...rows.map(s=>s.z))],
  max:[Math.max(...rows.map(s=>s.x)),Math.max(...rows.map(s=>s.z))],
});
const ownership = {};
for (const size of [64,128,256,512]) {
  const owners = new Map();
  for (const s of samples) {
    const key=`${Math.floor(s.x/size)},${Math.floor(s.z/size)}`;
    owners.set(key,(owners.get(key)??0)+1);
  }
  assert.equal([...owners.values()].reduce((a,b)=>a+b,0),1283);
  ownership[size] = {xz_tiles:owners.size, placements:1283};
}

const sectorPath='projects/world_demo/scenes/streaming/StreamMountain/objects/WorldSector.js';
const worldPath='projects/world_demo/scenes/streaming/StreamMountain/StreamMountain.js';
const propsPath='projects/world_demo/scenes/streaming/StreamMountain/props.json';
for (const p of [sectorPath,worldPath,propsPath])
  assert.equal(source(vg,p),source(relocation,p));
const {HABITAT}=await load(source(vg,'projects/world_demo/shared-lib/alpine_ecology.js'));
const {candidatesInRect}=await load(source(vg,'MatterEngine3/shared-lib/scatter_grid.js'));
const {rng}=await load(source(vg,'MatterEngine3/shared-lib/rng.js'));
const {mountainRockReferenceForSize}=await load(source(vg,'projects/world_demo/shared-lib/mountain_rock_sizes.js'));
const plannerSource=source(vg,'projects/world_demo/shared-lib/mountain_rock_scatter.js');
const stripImports=s=>s.replace(/^import[\s\S]*?;\s*/gm,'');
const {planMountainRocks,mountainRockClearance,MOUNTAIN_ROCK_HALO}=new Function(
  'HABITAT','mountainRockReferenceForSize',stripImports(plannerSource).replace(/^export /gm,'')+
  '\nreturn {planMountainRocks,mountainRockClearance,MOUNTAIN_ROCK_HALO};')(HABITAT,mountainRockReferenceForSize);
// Controlled flat habitat fixture, NOT a real terrain/residency census.
// Count the same 25 cells for both planners, with forest planning suppressed.
const naturalCensus=(commit,sector,modern)=>{
  const rows=[];
  class Part {
    terrainVolumeTiled() {}
    heightAt(x,z) {return 70+.05*x-.025*z;}
    slopeAt() {return .15;}
    biomeAt() {return 'foothills';}
    hasHabitat() {return true;}
    habitatAt(x,z,out) {out[HABITAT.altitude]=70;out[HABITAT.slope]=.15;}
    planCandidates() {return [];}
    pushMatrix() {this.pose=[];}
    translate(...p) {this.pose=p;}
    rotateX() {} rotateY() {} rotateZ() {} scale() {} popMatrix() {}
    placeChild(module,params) {rows.push({module,params,x:this.pose[0]+this.ox,z:this.pose[2]+this.oz});}
  }
  const bindings={Part,MAT:{grass:1,dirt:2,rock:3,snow:4},rng,candidatesInRect,
    isAlpineProfile:()=>true,planAlpineSector:()=>[],selectVegetationCatalog:()=>[],
    mountainRockCatalog:()=>[],mountainGeometrySamples:()=>[],mountainGeometryCatalog:()=>[],
    planMountainRocks,mountainRockClearance,MOUNTAIN_ROCK_HALO,
    mountainForestCatalog:()=>[],planMountainForest:()=>[],MOUNTAIN_FOREST_MIN_LOD:2};
  const Sector=new Function(...Object.keys(bindings),stripImports(source(commit,sector))+
    '\nreturn WorldSector;')(...Object.values(bindings));
  const counts={};
  for (const rung of [0,1,2]) {
    rows.length=0;
    for(let tz=20;tz<25;tz++)for(let tx=4;tx<9;tx++) {
      const p=new Sector();p.ox=tx*64;p.oz=tz*64;
      p.build({tx,tz,ty:0,volumetric:0,sectorSize:64,terrainLod:5,rung,worldSeed:20260722,
        biomes:JSON.stringify({__terrain:{material:'dirt'},__vegetation:{profile:'alpine-lush'},foothills:{rocks:16}})});
    }
    counts[rung]={placements:rows.length,sha256:sha256(JSON.stringify(rows))};
    assert.deepEqual([...new Set(rows.map(p=>p.module))],[modern?'MountainRock':'Rock']);
  }
  return {rectangle:{min:[256,1280],max_exclusive:[576,1600]},cells:25,world_seed:20260722,
    habitat:{altitude:70,slope:.15,biome:'foothills',height_plane:'70 + 0.05*x - 0.025*z'},
    forest:'suppressed',geometry_site:'excluded',scatter_rungs:counts};
};

const children=[
  ['1','c9f0a4566'],['2','1b54a566f'],['3','fc124643f'],['4','8a7e72f2b'],
  ['5','df8b673b9'],['6','2f328bd64'],['7','794c029a2'],['8','a21ffaa8b'],
  ['9','7e65df70e'],['10','6c70efe2e'],
].map(([id,short])=>{
  const head=git('rev-parse',short);
  assert.ok(ancestor(head,vg));assert.ok(!ancestor(head,main));
  return {task:`clear-ridge.${id}`,head,ancestor_of_vg:true,ancestor_of_main:false};
});
const implementation=git('rev-parse','46cb2266');
assert.ok(ancestor(implementation,vg));
const afterImplementation=git('diff','--name-only',implementation,vg).split('\n');
assert.ok(afterImplementation.every(p=>p.startsWith('docs/')));
const branchFiles=git('diff','--name-only',`${main}...${vg}`).split('\n');
const categories={};
for (const p of branchFiles) {const category=p.split('/')[0];categories[category]=(categories[category]??0)+1;}
const evidenceHash=JSON.parse(source(vg,'docs/agent/evidence/2026-09-18-stream-mountain-geometry/artifact-hashes.json'));
let mergeProbe;
try {
  mergeProbe={exit_code:0,stdout:git('merge-tree','--write-tree','--name-only',main,vg)};
} catch (e) {
  assert.equal(e.status,1,'merge probe must produce a valid conflicted tree');
  mergeProbe={exit_code:e.status,stdout:e.stdout.trim()};
}
const mergeLines=mergeProbe.stdout.split('\n');
const mergeConflicts=mergeLines.slice(1,mergeLines.indexOf('')).filter(Boolean);
const report={
  task:'quick-meadow-68.1',snapshot_date:'2026-09-30',
  heads:{main,vg,merge_base:base,stale_local_vg:stale,final_implementation:implementation},
  unique_commits:{main:Number(git('rev-list','--count',`${vg}..${main}`)),
    vg:Number(git('rev-list','--count',`${main}..${vg}`)),after_stale:Number(git('rev-list','--count',`${stale}..${vg}`))},
  branch_delta:{from_merge_base:git('diff','--shortstat',`${main}...${vg}`),
    between_heads:git('diff','--shortstat',main,vg),files_by_top_directory:categories,
    commits:git('log','--reverse','--format=%H %s',`${main}..${vg}`).split('\n'),
    main_only_commits:git('log','--reverse','--format=%H %s',`${vg}..${main}`).split('\n')},
  prospective_merge:{delivered:false,exit_code:mergeProbe.exit_code,
    tree:mergeLines[0],conflicting_paths:mergeConflicts},
  clear_ridge_delivery_heads:children,
  historical_pre_stress:{evidence:'docs/agent/evidence/2026-09-18-stream-mountain-geometry/README.md',
    git_commit_available:false,placements:3,resolution:96,source_triangles_per_rock:12*96*96,
    placed_source_triangles:3*12*96*96,source_sha256:evidenceHash[path],
    exact_earlier_pose_source_available:false},
  current_stress_site:{introduced_content_commit:content,scene_hookup_commit:relocation,
    placements:samples.length,catalog_assets:19,resolution:128,source_triangles_per_rock:12*128*128,
    placed_source_triangles:samples.length*12*128*128,unique_source_triangles:19*12*128*128,
    xz_bounds:bounds(samples),ownership,grid:{small:1024,large:256,spacing:8,axis_count:32,
      origin:[300,1320],small_jitter_width:5,small_hash_salt:'0x71a3',large_hash_salt:'0x4b21',
      small_seeds:[108,109,110,111,112,113,114,115],large_seeds:[100,101,102,103,104,105,106,107],
      small_sizes:[.25,.5,.75,1,1.25,1.5,1.75,2],large_sizes:[3,4.5,6,7.5]},world_seed_dependency:false},
  proposed_population_target:{status:'inferred from historical count and current three-record prefix; implementation follows separately',
    placements:3,catalog_assets:3,removed_grid_placements:1280,resolution:128,
    placed_source_triangles:3*12*128*128,focal_records:focal,material_id_in_this_fixture:7,
    material_in_production:'Mountain.GeometryRock'},
  natural_scatter_control:{
    older_git_population:naturalCensus(base,'projects/world_demo/scenes/StreamMountain/objects/WorldSector.js',false),
    current_population:naturalCensus(vg,sectorPath,true),
    interpretation:'Different planners; do not treat these flat-fixture counts as real-world or pre-stress scene counts.'},
  current_saved_props:JSON.parse(source(vg,propsPath)),
  source_sha256:Object.fromEntries([path,worldPath,sectorPath,propsPath,
    'projects/world_demo/shared-lib/mountain_detail_rocks.js',
    'projects/world_demo/shared-lib/mountain_rock_scatter.js',
    'projects/world_demo/shared-lib/mountain_forest.js'].map(p=>[p,sha256(source(vg,p))])),
};
console.log(JSON.stringify(report,null,2));
