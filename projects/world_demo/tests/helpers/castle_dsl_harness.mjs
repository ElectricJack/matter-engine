// Exercise authored JS with the real Part prelude and recorded native calls.
// Run Node with --experimental-vm-modules. This validates the authoring contract;
// actual meshing, material transport and rendered output still require the editor.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const repository = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../../..');
const prelude = fs.readFileSync(path.join(repository, 'MatterEngine3/src/part_base.js.h'), 'utf8')
  .split('R"JS(')[1].split(')JS"')[0];
const nativeNames = [...new Set(prelude.match(/__dsl_\w+/g))];
const canonical = value => JSON.stringify(Object.fromEntries(Object.entries(value || {}).sort(([a],[b]) => a.localeCompare(b))));

export async function createCastleHarness(sceneName, options = {}) {
  const project = path.join(repository, 'projects/world_demo');
  const scene = path.join(project, 'scenes', sceneName);
  const materials = [];
  const materialByName = new Map();
  let active = null;
  const context = vm.createContext({
    World: class {},
    defineMaterial(name, spec) {
      const existing = materialByName.get(name);
      if (existing) {
        assert.equal(canonical(existing.spec), canonical(spec), `conflicting material ${name}`);
        return existing.id;
      }
      const id = (options.materialBase ?? 40) + materials.length;
      const record = { id, name, spec };
      materials.push(record);
      materialByName.set(name, record);
      return id;
    },
  });
  for (const name of nativeNames) {
    context[name] = (...args) => {
      assert.ok(active, `DSL ${name} must occur inside build()`);
      function finite(value) {
        if (typeof value === 'number') assert.ok(Number.isFinite(value), `${active.module}: nonfinite ${name}`);
        else if (Array.isArray(value)) value.forEach(finite);
      }
      args.forEach(finite);
      active.calls[name] = (active.calls[name] || 0) + 1;
      const pairs = {
        __dsl_pushMatrix: ['matrix', 1], __dsl_popMatrix: ['matrix', -1],
        __dsl_beginShape: ['shape', 1], __dsl_endShape: ['shape', -1],
        __dsl_beginVoxels: ['voxel', 1], __dsl_endVoxels: ['voxel', -1],
        __dsl_beginModifier: ['modifier', 1], __dsl_endModifier: ['modifier', -1],
      };
      if (pairs[name]) {
        const [kind, delta] = pairs[name];
        active.depth[kind] += delta;
        assert.ok(active.depth[kind] >= 0, `${active.module}: unbalanced ${kind}`);
      }
      if (name === '__dsl_placeChild') {
        const [module, params] = args;
        const declared = active.requires.find(r => r.module === module && canonical(r.params) === canonical(params));
        assert.ok(declared, `${active.module}: undeclared child ${module} ${canonical(params)}`);
        active.children.push({ module, params: params || {} });
      }
      return undefined;
    };
  }
  vm.runInContext(prelude, context, { filename: 'part_base.js.h' });
  const modules = new Map();
  function sharedFile(specifier) {
    const leaf = specifier.slice('shared-lib/'.length);
    assert.ok(!leaf.includes('/') && !leaf.includes('..'), 'shared imports must be flat');
    const name = leaf.endsWith('.js') ? leaf : leaf + '.js';
    const choices = [path.join(project, 'shared-lib', name), path.join(repository, 'MatterEngine3/shared-lib', name)];
    return choices.find(p => fs.existsSync(p)) || choices[0];
  }
  function load(file, entry = false) {
    const key = file + (entry ? ':entry' : '');
    if (modules.has(key)) return modules.get(key);
    const source = fs.readFileSync(file, 'utf8');
    const name = path.basename(file, '.js');
    const module = new vm.SourceTextModule(source + (entry ? `\nexport default ${name};\n` : ''), { context, identifier: file });
    modules.set(key, module);
    return module;
  }
  async function entry(file) {
    const module = load(file, true);
    // Let vm link the entire dependency graph. Recursively starting link() in
    // each child races on diamond imports and returns half-linked modules.
    if (module.status === 'unlinked') await module.link(specifier => {
      assert.ok(specifier.startsWith('shared-lib/'), `unsupported import ${specifier}`);
      return load(sharedFile(specifier));
    });
    if (module.status !== 'evaluated') await module.evaluate();
    return module.namespace.default;
  }
  const world = await entry(path.join(scene, sceneName + '.js'));
  async function build(module, params = {}) {
    assert.equal(active, null, 'harness builds sequentially');
    const choices = [path.join(scene, 'objects', module + '.js'), path.join(project, 'objects', module + '.js')];
    const file = choices.find(p => fs.existsSync(p));
    assert.ok(file, `missing part module ${module}`);
    const ctor = await entry(file);
    const merged = { ...ctor.params, ...params };
    for (const value of Object.values(merged))
      assert.ok(['number','string','boolean'].includes(typeof value), `${module}: non-scalar part param`);
    const requires = typeof ctor.requires === 'function' ? ctor.requires(merged) : ctor.requires || [];
    const record = { module, params: merged, requires, calls: {}, children: [], depth: {matrix:0,shape:0,voxel:0,modifier:0} };
    active = record;
    try { new ctor().build(merged); }
    finally { active = null; }
    for (const [kind,depth] of Object.entries(record.depth)) assert.equal(depth, 0, `${module}: unclosed ${kind}`);
    return record;
  }
  return { world, materials, build, repository, scene };
}
