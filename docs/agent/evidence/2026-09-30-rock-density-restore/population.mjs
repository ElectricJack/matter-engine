// Deterministic before/after population comparison for the StreamMountain rock
// density restoration (quick-meadow-68.3). Both scene trees are extracted from
// Git (or the after side read from this checkout) and evaluated with their own
// shared-lib modules: the real StreamMountain biomes(), WorldSector requires()
// and build(), and the real natural rock / forest planners. Terrain is a
// synthetic analytic fixture, so natural counts compare the two trees rather
// than census the real mountain. Launches no editor, C++ suite or GPU work.
//
//   node docs/agent/evidence/2026-09-30-rock-density-restore/population.mjs [after-rev] [--write]
//
// after-rev defaults to the working tree; --write refreshes population.json.
import assert from 'node:assert/strict';
import {execFileSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {register} from 'node:module';
import {fileURLToPath, pathToFileURL} from 'node:url';

const repo = fileURLToPath(new URL('../../../../', import.meta.url));
const git = (...args) => execFileSync('git', args, {cwd:repo, encoding:'utf8'}).trim();
const sha256 = value => createHash('sha256')
  .update(typeof value === 'string' ? value : JSON.stringify(value)).digest('hex');
const BEFORE = '563945f01de2ae237a0c328abd5245566e960a0b'; // quick-meadow-68.2 / VG head
const args = process.argv.slice(2);
const write = args.includes('--write');
const afterRev = args.find(a => !a.startsWith('--')) ?? 'worktree';
const SCENE = 'projects/world_demo/scenes/streaming/StreamMountain';
const SITE = 'projects/world_demo/shared-lib/mountain_geometry_site.js';
const TREES = ['projects/world_demo/shared-lib', 'MatterEngine3/shared-lib', SCENE];

const extract = rev => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'rock-density-'));
  const archive = execFileSync('git', ['archive', '--format=tar', git('rev-parse', rev), ...TREES],
    {cwd:repo, maxBuffer:1 << 30});
  execFileSync('tar', ['-x', '-C', dir], {input:archive});
  return dir;
};
const trees = {before:extract(BEFORE), after:afterRev === 'worktree' ? repo : extract(afterRev)};
const read = (side, file) => fs.readFileSync(path.join(trees[side], file), 'utf8');

// The engine's flat `shared-lib/<name>` resolution, per tree: a module imported
// from the before tree only ever sees before-tree shared libraries.
const roots = Object.values(trees).map(root => [
  pathToFileURL(path.join(root, 'projects/world_demo/shared-lib') + '/').href,
  pathToFileURL(path.join(root, 'MatterEngine3/shared-lib') + '/').href]);
register('data:text/javascript,' + encodeURIComponent(`
import fs from 'node:fs';
let roots = [];
export async function initialize(data) { roots = data.roots; }
export async function resolve(specifier, context, next) {
  if (!specifier.startsWith('shared-lib/')) return next(specifier, context);
  const tree = roots.find(r => r.some(root => context.parentURL?.startsWith(root)));
  if (!tree) throw new Error('unscoped shared-lib import ' + specifier);
  const leaf = specifier.slice(11), name = leaf.endsWith('.js') ? leaf : leaf + '.js';
  for (const root of tree) {
    const url = new URL(name, root);
    if (fs.existsSync(url)) return {url:url.href, format:'module', shortCircuit:true};
  }
  throw new Error('shared-lib module not found: ' + specifier);
}
export async function load(url, context, next) {
  if (roots.flat().some(root => url.startsWith(root)))
    return next(url, {...context, format:'module'});
  return next(url, context);
}`), import.meta.url, {data:{roots}});

const sharedUrl = (side, leaf) => {
  const name = leaf.endsWith('.js') ? leaf : leaf + '.js';
  for (const dir of ['projects/world_demo/shared-lib', 'MatterEngine3/shared-lib']) {
    const file = path.join(trees[side], dir, name);
    if (fs.existsSync(file)) return pathToFileURL(file).href;
  }
  throw new Error('missing ' + leaf);
};
// Scene scripts are not ES modules: bind their shared-lib imports and globals.
const evaluate = async (side, file, globals, name) => {
  const text = read(side, file), bindings = {...globals};
  for (const m of text.matchAll(/^import\s*\{([^}]*)\}\s*from\s*'shared-lib\/([^']+)';/gm)) {
    const mod = await import(sharedUrl(side, m[2]));
    for (const spec of m[1].split(',').map(s => s.trim()).filter(Boolean)) {
      const [imported, local = imported] = spec.split(/\s+as\s+/);
      assert.ok(imported in mod, `${file} imports ${imported}`);
      bindings[local] = mod[imported];
    }
  }
  return new Function(...Object.keys(bindings),
    text.replace(/^import[\s\S]*?;\s*/gm, '') + `\nreturn ${name};`)(...Object.values(bindings));
};

// Synthetic, smooth terrain and habitat. Not the native mountain field.
const height = (x, z) => 70 + .05*x - .025*z + 3*Math.sin(x/37)*Math.cos(z/53);
const habitat = (x, z, out) => {
  out.fill(.5);
  out[0] = height(x, z);
  out[1] = Math.min(.45, Math.hypot(.05 + 3/37*Math.cos(x/37)*Math.cos(z/53),
    -.025 - 3/53*Math.sin(x/37)*Math.sin(z/53)));
  out[2] = .55 + .25*Math.sin(x/90);
  out[5] = .55 + .35*Math.sin(x/70 + z/110);
  out[6] = .3 + .2*Math.cos(z/40);
};
const census = async side => {
  const material = {};
  const World = await evaluate(side, `${SCENE}/StreamMountain.js`,
    {World:class {}, defineMaterial:name => (material[name] = 1000 + Object.keys(material).length)},
    'StreamMountain');
  const biomes = JSON.stringify(new World().biomes());
  const params = World.params, streaming = World.streaming;
  const rows = [];
  class Part {
    constructor() { this.stack = []; this.ops = []; }
    terrainVolumeTiled() {}
    heightAt(x, z) { return height(x, z); }
    slopeAt() { return .15; }
    biomeAt() { return 'foothills'; }
    hasHabitat() { return true; }
    habitatAt(x, z, out) { habitat(x, z, out); }
    pushMatrix() { this.stack.push(this.ops); this.ops = []; }
    popMatrix() { this.ops = this.stack.pop(); }
    translate(...v) { this.ops.push(['t', ...v]); }
    rotateX(a) { this.ops.push(['rx', a]); }
    rotateY(a) { this.ops.push(['ry', a]); }
    rotateZ(a) { this.ops.push(['rz', a]); }
    scale(...v) { this.ops.push(['s', ...v]); }
    placeChild(module, childParams, options) {
      const [, x, y, z] = this.ops[0];
      rows.push({module, params:childParams, options:options ?? null, ops:this.ops,
        x:x + this.tile[0], y:y + this.tile[1], z:z + this.tile[2]});
    }
  }
  const Sector = await evaluate(side, `${SCENE}/objects/WorldSector.js`,
    {Part, MAT:{grass:1, dirt:2, rock:3, snow:4}}, 'WorldSector');
  const requires = Sector.requires({biomes});
  // Every nested level over the site area, cube tiles exactly as streamed
  // (volumetricSectors), every vertical tile in the world's slab.
  const levels = {};
  for (const level of [0, 1, 2, 3]) {
    const size = 64 << level, x0 = Math.floor(256/size), x1 = Math.floor(575/size);
    const z0 = Math.floor(1280/size), z1 = Math.floor(1599/size);
    rows.length = 0;
    for (let tz = z0; tz <= z1; tz++)
    for (let tx = x0; tx <= x1; tx++)
    for (let ty = Math.floor(-96/size); ty <= Math.floor(703/size); ty++) {
      const sector = new Sector();
      sector.tile = [tx*size, ty*size, tz*size];
      sector.build({tx, ty, tz, rung:2, terrainLod:5, volumetric:1, sectorSize:size,
        worldSeed:params.worldSeed, fieldHash:'', biomes});
    }
    // Emission order follows tile iteration; compare the placed set.
    const site = rows.filter(r => r.module === 'MountainDetailRock')
      .map(r => [r.x, r.y, r.z, r.params, r.options])
      .sort((a, b) => a[0] - b[0] || a[2] - b[2]);
    const natural = rows.filter(r => r.module !== 'MountainDetailRock');
    const modules = {};
    for (const r of rows) modules[r.module] = (modules[r.module] ?? 0) + 1;
    levels[size] = {tiles_xz:{min:[x0*size, z0*size], max_exclusive:[(x1 + 1)*size, (z1 + 1)*size]},
      placements:rows.length, modules,
      site:{placements:site.length, sha256:sha256(site)},
      natural:{placements:natural.length,
        sha256:sha256(natural.map(r => [r.module, r.params, r.ops, r.x, r.y, r.z]))}};
  }
  const {mountainGeometrySamples} = await import(sharedUrl(side, 'mountain_geometry_site'));
  const samples = mountainGeometrySamples(material['Mountain.GeometryRock'],
    {stress:params.geometryRockStress === true});
  const assets = [...new Set(samples.map(s => JSON.stringify(s.params)))];
  const triangles = r => 12*r*r;
  return {
    world_params:params, volumetric_sectors:streaming.volumetricSectors,
    terrain_texels_per_meter:streaming.terrainTexelsPerMeter,
    biomes, requires:{total:requires.length,
      detail_rocks:requires.filter(r => r.module === 'MountainDetailRock').length,
      other_sha256:sha256(requires.filter(r => r.module !== 'MountainDetailRock'))},
    site:{placements:samples.length, unique_assets:assets.length,
      focal:samples.slice(0, 3).map(s => [s.x, s.z, s.params.seed, s.params.shape, s.params.size, s.params.resolution]),
      resolutions:[...new Set(samples.map(s => s.params.resolution))],
      placed_source_triangles:samples.reduce((n, s) => n + triangles(s.params.resolution), 0),
      unique_source_triangles:assets.reduce((n, a) => n + triangles(JSON.parse(a).resolution), 0),
      sha256:sha256(samples)},
    levels,
  };
};

const before = await census('before'), after = await census('after');
const {mountainGeometrySamples:stressSite} = await import(sharedUrl('after', 'mountain_geometry_site'));
const stress = stressSite(JSON.parse(before.biomes).__geometryRocks.material, {stress:true});

// Only the inspection-site population changes.
assert.equal(before.site.placements, 1283);
assert.equal(before.site.unique_assets, 19);
assert.equal(after.site.placements, 3);
assert.equal(after.site.unique_assets, 3);
assert.deepEqual(after.site.focal, before.site.focal);
assert.deepEqual(after.site.resolutions, [128]);
assert.equal(after.world_params.geometryRockStress, false);
assert.equal(sha256(stress), before.site.sha256, 'opt-in stress profile reproduces the old records');
assert.equal(after.requires.detail_rocks, 3);
assert.equal(before.requires.detail_rocks, 19);
assert.equal(after.requires.other_sha256, before.requires.other_sha256, 'natural/forest catalogs unchanged');
for (const size of Object.keys(before.levels)) {
  const b = before.levels[size], a = after.levels[size];
  assert.equal(b.site.placements, 1283, `before: one owner per rock at ${size} m`);
  assert.equal(a.site.placements, 3, `after: one owner per rock at ${size} m`);
  assert.deepEqual(a.natural, b.natural, `natural rock/forest placements unchanged at ${size} m`);
  assert.equal(b.site.sha256, before.levels[64].site.sha256, 'before: site placements stable across levels');
  assert.equal(a.site.sha256, after.levels[64].site.sha256, 'after: site placements stable across levels');
}
// Terrain/material/POM/streaming settings are not part of this change.
for (const key of ['worldSeed', 'terrainOnly'])
  assert.equal(after.world_params[key], before.world_params[key]);
assert.equal(after.terrain_texels_per_meter, before.terrain_texels_per_meter);
assert.equal(after.volumetric_sectors, before.volumetric_sectors);
assert.equal(read('after', `${SCENE}/props.json`), read('before', `${SCENE}/props.json`));
const beforeBiomes = JSON.parse(before.biomes), afterBiomes = JSON.parse(after.biomes);
assert.equal(afterBiomes.__geometryRockStress, false);
delete afterBiomes.__geometryRockStress;
assert.deepEqual(afterBiomes, beforeBiomes);

const changed = afterRev === 'worktree'
  ? git('diff', '--name-only', BEFORE, '--', ...TREES) : git('diff', '--name-only', BEFORE, afterRev, '--', ...TREES);
const report = {
  task:'quick-meadow-68.3', before_rev:BEFORE, after_rev:afterRev,
  changed_scene_and_shared_lib_files:changed.split('\n').filter(Boolean),
  fixture:{terrain:'70 + 0.05x - 0.025z + 3 sin(x/37) cos(z/53) (synthetic)', biome:'foothills',
    area:{min:[256, 1280], max_exclusive:[576, 1600], each_level:'every tile overlapping area'},
    rung:2, terrainLod:5, volumetric:1,
    note:'natural counts compare the two trees on a synthetic field; they are not a real-terrain census'},
  before, after,
};
const out = new URL('population.json', import.meta.url);
if (write) fs.writeFileSync(out, JSON.stringify(report, null, 2) + '\n');
for (const side of ['before', 'after'])
  if (trees[side] !== repo) fs.rmSync(trees[side], {recursive:true, force:true});
const levelSummary = side => Object.entries(report[side].levels)
  .map(([size, l]) => `${size}m site=${l.site.placements} natural=${l.natural.placements}`).join(', ');
console.log(`before ${BEFORE.slice(0, 9)}: ${before.site.placements} site placements / ` +
  `${before.site.unique_assets} assets; ${levelSummary('before')}`);
console.log(`after ${afterRev}: ${after.site.placements} site placements / ` +
  `${after.site.unique_assets} assets; ${levelSummary('after')}`);
console.log('PASS: only the detailed-rock inspection site changed' + (write ? '; wrote population.json' : ''));
