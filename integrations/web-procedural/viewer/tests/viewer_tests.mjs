// Browser fixtures for the web viewer adapter: pixel checks against a real
// WebGL2 context, GPU resource accounting through instrumented GL entry points,
// contract error codes, and end-to-end regeneration through the real worker
// runtime and WASM geometry kernel. Run by tests/browser_tests.py.
import * as THREE from 'three';
import {createAssetView, ViewerError, CAPABILITY_MATRIX, NATIVE_ONLY_FEATURES} from '../dist/index.mjs';
import {DemoSession} from '../dist/demo/session.mjs';
import {FIXTURES, fixture, defaults} from '../dist/demo/fixtures.mjs';
import {ProceduralRuntime} from '../../runtime/dist/index.mjs';

// ---------------------------------------------------------------------------
// Instrumentation: live GL objects and dispose() calls. Installed before the
// renderer exists so every allocation is seen.
const GL_KINDS = ['Buffer', 'Texture', 'VertexArray', 'Program'];
const live = Object.fromEntries(GL_KINDS.map(kind => [kind, new Set()]));
for (const kind of GL_KINDS) {
  const proto = WebGL2RenderingContext.prototype;
  const create = proto[`create${kind}`], remove = proto[`delete${kind}`];
  proto[`create${kind}`] = function (...args) { const object = create.apply(this, args); if (object) live[kind].add(object); return object; };
  proto[`delete${kind}`] = function (object) { live[kind].delete(object); return remove.call(this, object); };
}
const glCounts = () => Object.fromEntries(GL_KINDS.map(kind => [kind, live[kind].size]));
const disposeCalls = {geometry: 0, material: 0, texture: 0};
for (const [key, Class] of [['geometry', THREE.BufferGeometry], ['material', THREE.Material], ['texture', THREE.Texture]]) {
  const dispose = Class.prototype.dispose;
  Class.prototype.dispose = function () { disposeCalls[key] += 1; return dispose.call(this); };
}

// ---------------------------------------------------------------------------
// Assertions and contract documents.
function check(condition, message) { if (!condition) throw new Error(message); }
function near(a, b, tolerance, message) { check(Math.abs(a - b) <= tolerance, `${message}: ${a} vs ${b} (±${tolerance})`); }
function same(a, b, message) { check(JSON.stringify(a) === JSON.stringify(b), `${message}: ${JSON.stringify(a)} vs ${JSON.stringify(b)}`); }

const I4 = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
const T = (x, y, z) => [1, 0, 0, x, 0, 1, 0, y, 0, 0, 1, z, 0, 0, 0, 1];
/** Row-major a * b; independent of the adapter's matrix module. */
function mul(a, b) {
  const out = new Array(16).fill(0);
  for (let r = 0; r < 4; ++r) for (let c = 0; c < 4; ++c) for (let k = 0; k < 4; ++k) out[r * 4 + c] += a[r * 4 + k] * b[k * 4 + c];
  return out;
}
const at = (m, x, y, z) => [0, 1, 2].map(r => m[r * 4] * x + m[r * 4 + 1] * y + m[r * 4 + 2] * z + m[r * 4 + 3]);
const rotY180 = [-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1];
const mirrorX = (x, y, z) => mul(T(x, y, z), [-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);
const SHEAR = [1, 0, 0, 0, 0, 1, 0, 0, 0.5, 0, 1, 0, 0, 0, 0, 1];  // z' = z + 0.5 x

const u64hex = n => BigInt(n).toString(16).padStart(16, '0');
async function sha256(bytes) {
  return Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256', bytes)), x => x.toString(16).padStart(2, '0')).join('');
}

function material(id, extra = {}) {
  const {factor = [1, 1, 1, 1], texture = null, uvSet = 0, ...rest} = extra;
  return {id, base_color: {factor, texture, uv_set: uvSet}, metallic: 0, roughness: 1, normal_scale: 1, emissive: [0, 0, 0],
    metallic_roughness_texture: null, normal_texture: null, emissive_texture: null, ...rest};
}

function bounds(positions) {
  const min = [0, 1, 2].map(k => Math.min(...positions.map(p => p[k])));
  const max = [0, 1, 2].map(k => Math.max(...positions.map(p => p[k])));
  return {min, max};
}

/** Unit quad in the XY plane facing +Z, UV = (x, y), CCW from +Z. */
function quad(id, materialId, options = {}) {
  const positions = options.positions ?? [[0, 0, 0], [1, 0, 0], [1, 1, 0], [0, 1, 0]];
  return {id, positions, normals: options.normals ?? positions.map(() => [0, 0, 1]), tangents: options.tangents ?? [],
    uv_sets: options.uvSets ?? [[[0, 0], [1, 0], [1, 1], [0, 1]]], indices: options.indices ?? [0, 1, 2, 0, 2, 3],
    submeshes: [{first_index: 0, index_count: 6, material: materialId}], winding: 'ccw', bounds: bounds(positions)};
}

const place = (id, prototype, transform = I4, target = 'mesh', parent = null) =>
  ({id, prototype, target, parent, local_transform: transform});

function doc(over = {}) {
  return {document_type: 'asset', canonical_profile: 'matter-exact-json-v1', schema_version: {major: 1, minor: 0},
    id: 'test.asset', bounds: {min: [-10, -10, -10], max: [10, 10, 10]},
    required_capabilities: [{name: 'matter.asset.core', version: 1}, {name: 'matter.material.pbr', version: 1}],
    resources: [], materials: [material('m')], textures: [], extensions: [],
    meshes: [quad('q', 'm')], assemblies: [], instances: [place('i0', 'q')],
    attachments: [], lods: [], collisions: [], rigs: [], skins: [], animations: [], terrain: null, metadata: {}, provenance: {},
    ...over};
}

/** An rgba8 texture plus its resource descriptor and bytes. `rows` is [row][column] of RGBA. */
async function texture(id, rows, options = {}) {
  const height = rows.length, width = rows[0].length, bytes = new Uint8Array(rows.flat(2));
  const resource = `${id}.texels`;
  return {
    texture: {id, resource, color_space: options.colorSpace ?? 'srgb', wrap_u: 'clamp', wrap_v: 'clamp',
      min_filter: 'nearest', mag_filter: 'nearest', width, height, pixel_format: 'rgba8'},
    descriptor: {id: resource, content: {algorithm: 'sha256', hex: await sha256(bytes)}, scalar_type: 'u8', components: 4,
      element_count: u64hex(width * height), byte_length: u64hex(bytes.length)},
    bytes: {id: resource, bytes},
  };
}

// ---------------------------------------------------------------------------
// Renderer and orthographic pixel probes. readPixels has a bottom-left origin.
const SIZE = 96;
let renderer, scene, camera, gl, baseline;
const view = {cx: 0, cy: 0, half: 1};

function setupRenderer() {
  const canvas = document.createElement('canvas');
  canvas.width = SIZE; canvas.height = SIZE;
  document.body.append(canvas);
  renderer = new THREE.WebGLRenderer({canvas, antialias: false, preserveDrawingBuffer: true});
  renderer.setPixelRatio(1);
  renderer.setSize(SIZE, SIZE, false);
  renderer.toneMapping = THREE.NoToneMapping;
  renderer.setClearColor(0xff00ff, 1);
  gl = renderer.getContext();
  scene = new THREE.Scene();
  camera = new THREE.OrthographicCamera(-1, 1, 1, -1, 0.1, 100);
  camera.position.set(0, 0, 10);
  // Warm-up: three r186 creates one module-global DFG LUT texture the first time
  // a PBR material renders and keeps it for the page's life. Allocate it before
  // the baseline so per-view accounting sees only the view's own objects.
  const warm = new THREE.Mesh(new THREE.PlaneGeometry(0.1, 0.1), new THREE.MeshStandardMaterial());
  scene.add(warm);
  renderer.render(scene, camera);
  scene.remove(warm);
  warm.geometry.dispose(); warm.material.dispose();
  renderer.render(scene, camera);
  baseline = glCounts();
}

function frameView(cx, cy, half) {
  Object.assign(view, {cx, cy, half});
  Object.assign(camera, {left: cx - half, right: cx + half, top: cy + half, bottom: cy - half});
  camera.updateProjectionMatrix();
}

function setLights(...lights) {
  for (const light of scene.children.filter(c => c.isLight)) scene.remove(light);
  for (const light of lights) scene.add(light);
}
const ambient = () => new THREE.AmbientLight(0xffffff, Math.PI);
function directional(x, y, z) { const light = new THREE.DirectionalLight(0xffffff, Math.PI); light.position.set(x, y, z); return light; }

function render() { renderer.render(scene, camera); }

function pixel(x, y) {
  const px = Math.floor((x - (view.cx - view.half)) / (2 * view.half) * SIZE);
  const py = Math.floor((y - (view.cy - view.half)) / (2 * view.half) * SIZE);
  const out = new Uint8Array(4);
  gl.readPixels(px, py, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, out);
  return [out[0], out[1], out[2]];
}
const isClear = p => p[0] > 245 && p[1] < 10 && p[2] > 245;
const close = (a, b, tolerance) => a.every((v, i) => Math.abs(v - b[i]) <= tolerance);

/** Show a view alone, render it, and return it (caller disposes). */
function show(source, options) {
  const v = createAssetView(source, options);
  scene.add(v.root);
  scene.updateMatrixWorld(true);
  render();
  return v;
}

/** Row-major world matrix three.js will draw for a placed instance. */
function drawnMatrix(instance) {
  const m = instance.object.matrixWorld.clone();
  if (instance.instanceIndex !== null) {
    const local = new THREE.Matrix4();
    instance.object.getMatrixAt(instance.instanceIndex, local);
    m.multiply(local);
  }
  const e = m.elements;
  return Array.from({length: 16}, (_, i) => e[(i % 4) * 4 + Math.floor(i / 4)]);
}

/** Independent flatten of a contract document: path -> {prototype, world}. */
function flatten(asset) {
  const out = new Map();
  const assemblies = new Map(asset.assemblies.map(a => [a.id, a.instances]));
  const walk = (instances, parentWorld, prefix) => {
    const byId = new Map(instances.map(x => [x.id, x]));
    const chain = x => (x.parent === null ? x.local_transform : mul(chain(byId.get(x.parent)), x.local_transform));
    for (const x of instances) {
      const world = mul(parentWorld, chain(x));
      if (x.target === 'assembly') walk(assemblies.get(x.prototype), world, `${prefix}${x.id}/`);
      else out.set(`${prefix}${x.id}`, {prototype: x.prototype, world});
    }
  };
  walk(asset.instances, I4, '');
  return out;
}

function checkMatrices(v, asset, label) {
  const expected = flatten(asset);
  check(expected.size === v.instances.size, `${label}: ${v.instances.size} placements, expected ${expected.size}`);
  for (const [path, {prototype, world}] of expected) {
    const instance = v.instances.get(path);
    check(instance && instance.prototype === prototype, `${label}: ${path} missing or wrong prototype`);
    const drawn = drawnMatrix(instance);
    for (let i = 0; i < 16; ++i)
      near(drawn[i], world[i], 1e-5 * (1 + Math.abs(world[i])), `${label}: ${path} matrix[${i}]`);
  }
}

// ---------------------------------------------------------------------------
// Pixel fixtures.
async function colorSpaceFixture(m) {
  const grey = await texture('grey.srgb', [[[128, 128, 128, 255]]]);
  const greyLinear = await texture('grey.linear', [[[128, 128, 128, 255]]], {colorSpace: 'linear'});
  // sRGB 128 decodes to linear 0.2158605; a linear texel 128 is 128/255.
  const srgbReference = 0.21586050011389926, linearReference = 128 / 255;
  const source = {asset: doc({
    resources: [grey.descriptor, greyLinear.descriptor], textures: [grey.texture, greyLinear.texture],
    materials: [material('srgb', {texture: 'grey.srgb'}), material('linear', {texture: 'grey.linear'}),
      material('srgb.ref', {factor: [srgbReference, srgbReference, srgbReference, 1]}),
      material('linear.ref', {factor: [linearReference, linearReference, linearReference, 1]})],
    meshes: ['srgb', 'linear', 'srgb.ref', 'linear.ref'].map(id => quad(`q.${id}`, id)),
    instances: ['srgb', 'linear', 'srgb.ref', 'linear.ref'].map((id, i) => place(`i.${id}`, `q.${id}`, T(2 * i, 0, 0))),
  }), resources: [grey.bytes, greyLinear.bytes]};
  frameView(3.5, 0.5, 4);
  setLights(ambient());
  const v = show(source);
  const [srgb, linear, srgbRef, linearRef] = [0, 1, 2, 3].map(i => pixel(2 * i + 0.5, 0.5));
  check(close(srgb, srgbRef, 2), `sRGB texel ${srgb} vs factor reference ${srgbRef}`);
  check(close(linear, linearRef, 2), `linear texel ${linear} vs factor reference ${linearRef}`);
  check(linear[0] - srgb[0] > 20, `sRGB and linear textures must decode differently (${srgb} vs ${linear})`);
  const map = v.instances.get('i.srgb').object.material.map;
  check(map.colorSpace === THREE.SRGBColorSpace && map.flipY === false, 'sRGB texture color space / flipY');
  check(v.instances.get('i.linear').object.material.map.colorSpace === THREE.NoColorSpace, 'linear texture color space');
  m['COLOR-SRGB'] = {srgb, srgbRef, linear, linearRef};
  v.dispose();
}

const RED = [255, 0, 0, 255], GREEN = [0, 255, 0, 255], BLUE = [0, 0, 255, 255], WHITE = [255, 255, 255, 255];
const dominant = (p, channel) => p[channel] > 150 && [0, 1, 2].every(k => k === channel || p[k] < 60);

async function orientationFixture(m) {
  // Row 0 is v = 0 (bottom), column 0 is u = 0 (left).
  const tex = await texture('orient', [[RED, GREEN], [BLUE, WHITE]]);
  const source = {asset: doc({resources: [tex.descriptor], textures: [tex.texture], materials: [material('m', {texture: 'orient'})]}),
    resources: [tex.bytes]};
  frameView(0.5, 0.5, 0.6);
  setLights(ambient());
  const v = show(source);
  const [bl, br, tl, tr] = [pixel(0.25, 0.25), pixel(0.75, 0.25), pixel(0.25, 0.75), pixel(0.75, 0.75)];
  check(dominant(bl, 0) && dominant(br, 1) && dominant(tl, 2) && tr.every(x => x > 200),
    `texture orientation BL ${bl} BR ${br} TL ${tl} TR ${tr}`);
  m['TEX-ORIENT'] = {bl, br, tl, tr};
  v.dispose();
}

async function uvSetFixture(m) {
  const tex = await texture('orient', [[RED, GREEN], [BLUE, WHITE]]);
  const zeros = [[0, 0], [0, 0], [0, 0], [0, 0]], uv = [[0, 0], [1, 0], [1, 1], [0, 1]];
  const source = {asset: doc({resources: [tex.descriptor], textures: [tex.texture],
    materials: [material('m', {texture: 'orient', uvSet: 1})], meshes: [quad('q', 'm', {uvSets: [zeros, uv]})]}), resources: [tex.bytes]};
  frameView(0.5, 0.5, 0.6);
  setLights(ambient());
  const before = renderer.info.memory.textures;
  const v = show(source);
  const object = v.instances.get('i0').object;
  check(object.geometry.getAttribute('uv1') && !object.geometry.getAttribute('uv'), 'only the sampled UV set (uv1) is uploaded');
  check(object.material.map.channel === 1, 'base color map samples channel 1');
  check(v.stats.textures === 1 && v.stats.textureObjects === 2, `texture stats ${JSON.stringify(v.stats)}`);
  check(renderer.info.memory.textures - before === 1, 'one GL texture for the base texture and its channel clone');
  const [bl, br, tl, tr] = [pixel(0.25, 0.25), pixel(0.75, 0.25), pixel(0.25, 0.75), pixel(0.75, 0.75)];
  check(dominant(bl, 0) && dominant(br, 1) && dominant(tl, 2) && tr.every(x => x > 200), `UV set 1 orientation ${[bl, br, tl, tr]}`);
  m.UVSET1 = {glTextures: renderer.info.memory.textures - before, stats: v.stats};
  v.dispose();
}

async function windingFixture(m) {
  // B is turned away from the camera and must be culled; mirrored C and D keep
  // their front faces toward the camera.
  const source = {asset: doc({instances: [place('A', 'q', T(-3, 0, 0)), place('B', 'q', mul(T(0, 0, 0), rotY180)),
    place('C', 'q', mirrorX(2, 0, 0)), place('D', 'q', mirrorX(4, 0, 0))]})};
  frameView(0.5, 0.5, 4);
  setLights(ambient());
  const v = show(source);
  const a = pixel(-2.5, 0.5), b = pixel(-0.5, 0.5), c = pixel(1.5, 0.5), d = pixel(3.5, 0.5);
  check(!isClear(a) && isClear(b) && !isClear(c) && !isClear(d), `winding A ${a} B ${b} C ${c} D ${d}`);
  same(v.stats.batches, {instanced: 1, mirroredInstanced: 1, individual: 0}, 'winding batches');
  const mirrored = v.instances.get('C').object;
  check(mirrored === v.instances.get('D').object && mirrored.isInstancedMesh && mirrored.matrixWorld.determinant() < 0,
    'mirrored placements share one batch with a negative object determinant');
  check(v.instances.get('A').object === v.instances.get('B').object, 'orthogonal placements share one batch');
  v.dispose();
  const single = show({asset: doc({instances: [place('M', 'q', mirrorX(1, 0, 0))]})});
  const s = pixel(0.5, 0.5);
  check(!isClear(s) && !single.instances.get('M').object.isInstancedMesh, `single mirrored placement visible ${s}`);
  m.WINDING = {a, b, c, d, single: s};
  single.dispose();
}

async function coordinatesFixture(m) {
  const response = await fetch('/libs/MatterContracts/examples/repeated-assembly.asset.json');
  check(response.ok, 'contract example fetch');
  const asset = await response.json();
  frameView(0, 2.5, 3.2);
  setLights(ambient());
  const v = show({asset});
  const lit = [[1 / 3, 1 / 3], [7 / 3, 1 / 3], [7 / 3, 3], [-5 / 3, 3], [2.7, 2.5]].map(([x, y]) => pixel(x, y));
  const empty = pixel(1.3, 4.0);
  check(lit.every(p => !isClear(p)) && isClear(empty), `contract centroids ${JSON.stringify(lit)} empty ${empty}`);
  same(v.stats.batches, {instanced: 1, mirroredInstanced: 1, individual: 0}, 'contract batches');
  check(v.stats.geometries === 1 && v.stats.instances === 4, `contract sharing ${JSON.stringify(v.stats)}`);
  checkMatrices(v, asset, 'COORDS');
  m.COORDS = {lit, empty};
  v.dispose();
}

async function shearFixture(m) {
  // Sheared placements must draw with a per-object normal matrix. R is the
  // same surface pre-sheared with analytic normals; F is the unsheared quad,
  // i.e. what instancing's column-scaled normal transform would produce.
  const n = 1 / Math.hypot(0.5, 1), shearedNormal = [-0.5 * n, 0, n];
  const sheared = [[0, 0, 0], [1, 0, 0.5], [1, 1, 0.5], [0, 1, 0]];
  const source = {asset: doc({materials: [material('m', {factor: [0.5, 0.5, 0.5, 1]})],
    meshes: [quad('q', 'm'), quad('r', 'm', {positions: sheared, normals: sheared.map(() => shearedNormal)})],
    instances: [place('S1', 'q', mul(T(-3, 0, 0), SHEAR)), place('S2', 'q', mul(T(-1.5, 0, 0), SHEAR)),
      place('F1', 'q', T(0, 0, 0)), place('F2', 'q', T(1.5, 0, 0)), place('R', 'r', T(3, 0, 0))]})};
  frameView(0.5, 0.5, 4);
  setLights(directional(-1, 0, 1));
  const v = show(source);
  const s1 = pixel(-2.5, 0.5), s2 = pixel(-1, 0.5), f = pixel(0.5, 0.5), r = pixel(3.5, 0.5);
  same(v.stats.batches, {instanced: 1, mirroredInstanced: 0, individual: 3}, 'shear batches');
  check(v.instances.get('S1').placement === 'general' && !v.instances.get('S1').object.isInstancedMesh, 'sheared placement drawn individually');
  check(close(s1, r, 3) && close(s2, r, 3), `sheared ${s1}/${s2} vs reference ${r}`);
  check(Math.abs(s1[0] - f[0]) > 10, `sheared ${s1} must differ from unsheared ${f}`);
  checkMatrices(v, source.asset, 'SHEAR');
  m.SHEAR = {s1, s2, f, r};
  v.dispose();
}

async function tangentFixture(m) {
  // A tangent-space normal tilted toward +X+Y. Under an X mirror the bitangent
  // must stay +Y, which needs the w-flipped tangent variant; R is the mirrored
  // surface authored directly (CCW, tangent -X, w = -1).
  const normalTexture = await texture('bump', [[[191, 191, 218, 255]]], {colorSpace: 'linear'});
  const tangents = [[1, 0, 0, 1], [1, 0, 0, 1], [1, 0, 0, 1], [1, 0, 0, 1]];
  const mirroredPositions = [[0, 0, 0], [-1, 0, 0], [-1, 1, 0], [0, 1, 0]];
  const asset = (instances) => doc({resources: [normalTexture.descriptor], textures: [normalTexture.texture],
    materials: [material('m', {factor: [0.5, 0.5, 0.5, 1], normal_texture: 'bump'})],
    meshes: [quad('p', 'm', {tangents}),
      quad('r', 'm', {positions: mirroredPositions, uvSets: [[[0, 0], [1, 0], [1, 1], [0, 1]]],
        tangents: tangents.map(() => [-1, 0, 0, -1]), indices: [0, 2, 1, 0, 3, 2]})],
    instances});
  frameView(1, 0.5, 4);
  setLights(directional(-1, 1, 1));
  const v = show({asset: asset([place('M1', 'p', mirrorX(-1.5, 0, 0)), place('M2', 'p', mirrorX(0.5, 0, 0)),
    place('O', 'p', T(1.5, 0, 0)), place('R', 'r', T(4, 0, 0))]), resources: [normalTexture.bytes]});
  const m1 = pixel(-2, 0.5), m2 = pixel(0, 0.5), o = pixel(2, 0.5), r = pixel(3.5, 0.5);
  check(close(m1, r, 3) && close(m2, r, 3), `mirrored normal map ${m1}/${m2} vs reference ${r}`);
  check(r[0] - o[0] > 30, `unmirrored placement ${o} must be darker than the mirrored reference ${r}`);
  check(v.stats.geometries === 3, `tangent geometries ${v.stats.geometries}`);
  same(v.stats.batches, {instanced: 0, mirroredInstanced: 1, individual: 2}, 'tangent batches');
  const base = v.instances.get('O').object.geometry, variant = v.instances.get('M1').object.geometry;
  check(base !== variant && variant.getAttribute('position') === base.getAttribute('position') && variant.index === base.index &&
    variant.getAttribute('tangent') !== base.getAttribute('tangent') && variant.getAttribute('tangent').array[3] === -1,
    'mirrored variant shares positions/index and flips tangent w');
  v.dispose();
  const single = show({asset: asset([place('M', 'p', mirrorX(1.5, 0, 0)), place('R', 'r', T(4, 0, 0))]), resources: [normalTexture.bytes]});
  const s = pixel(1, 0.5), r2 = pixel(3.5, 0.5);
  check(close(s, r2, 3), `single mirrored normal map ${s} vs reference ${r2}`);
  m['TANGENT-MIRROR'] = {mirrored: [m1, m2], orthogonal: o, reference: r, single: s};
  single.dispose();
}

// ---------------------------------------------------------------------------
// Disposal and error fixtures.
async function disposeFixture(m) {
  const orient = await texture('orient', [[RED, GREEN], [BLUE, WHITE]]);
  const bump = await texture('bump', [[[191, 191, 218, 255]]], {colorSpace: 'linear'});
  const tangents = [[1, 0, 0, 1], [1, 0, 0, 1], [1, 0, 0, 1], [1, 0, 0, 1]];
  const uv = [[0, 0], [1, 0], [1, 1], [0, 1]];
  const source = {asset: doc({resources: [orient.descriptor, bump.descriptor], textures: [orient.texture, bump.texture],
    materials: [material('a', {texture: 'orient', normal_texture: 'bump'}), material('b', {texture: 'orient', uvSet: 1})],
    meshes: [quad('p', 'a', {tangents, uvSets: [uv]}), quad('t', 'b', {uvSets: [uv, uv]})],
    instances: [place('P1', 'p', mirrorX(-1, 0, 0)), place('P2', 'p', mirrorX(1, 0, 0)), place('P3', 'p', T(2, 0, 0)),
      place('T1', 't', T(-3, 0, 0)), place('T2', 't', mul(T(3, 0, 0), SHEAR))]}), resources: [orient.bytes, bump.bytes]};
  frameView(0, 0.5, 4.5);
  setLights(ambient());
  render();
  const start = glCounts();
  same(start, baseline, 'GL objects before the dispose loop');
  let first = null;
  for (let i = 0; i < 20; ++i) {
    const info0 = {...renderer.info.memory}, calls0 = {...disposeCalls};
    const v = show(source);
    const s = v.stats;
    check(renderer.info.memory.geometries - info0.geometries === s.geometries, `iteration ${i}: geometries ${renderer.info.memory.geometries - info0.geometries} vs ${s.geometries}`);
    check(renderer.info.memory.textures - info0.textures === s.textures, `iteration ${i}: GL textures vs ${s.textures}`);
    const loaded = glCounts();
    if (first === null) first = {loaded, stats: s};
    else same(loaded, first.loaded, `iteration ${i}: GL objects while shown`);
    v.dispose();
    v.dispose();  // idempotent
    check(v.disposed && v.root.parent === null && v.root.children.length === 0, `iteration ${i}: root detached and cleared`);
    same({geometry: disposeCalls.geometry - calls0.geometry, material: disposeCalls.material - calls0.material, texture: disposeCalls.texture - calls0.texture},
      {geometry: s.geometries, material: s.materials, texture: s.textureObjects}, `iteration ${i}: dispose calls`);
    render();
    same(glCounts(), baseline, `iteration ${i}: GL objects after dispose`);
    check(renderer.info.memory.geometries === info0.geometries && renderer.info.memory.textures === info0.textures, `iteration ${i}: renderer.info after dispose`);
  }
  same(first.stats.batches, {instanced: 0, mirroredInstanced: 1, individual: 3}, 'dispose fixture batches');
  check(first.stats.geometries === 3 && first.stats.materials === 2 && first.stats.textures === 2 && first.stats.textureObjects === 3,
    `dispose fixture stats ${JSON.stringify(first.stats)}`);
  // A failure after materials exist releases them too (attachments are checked before geometry upload).
  const calls0 = {...disposeCalls};
  const bad = {...source, asset: {...source.asset, attachments: [{id: 'nowhere', instance: 'missing', local_transform: I4}],
    required_capabilities: [...source.asset.required_capabilities, {name: 'matter.asset.attachments', version: 1}]}};
  let failure = null;
  try { createAssetView(bad); } catch (error) { failure = error; }
  check(failure instanceof ViewerError && failure.code === 'missing_dependency', `failure release: ${failure}`);
  same({geometry: disposeCalls.geometry - calls0.geometry, material: disposeCalls.material - calls0.material, texture: disposeCalls.texture - calls0.texture},
    {geometry: 0, material: 2, texture: 3}, 'failure releases materials and textures');
  render();
  same(glCounts(), baseline, 'GL objects after a failed build');
  m.DISPOSE = {iterations: 20, baseline, loaded: first.loaded, stats: first.stats};
}

async function errorFixture(m) {
  const tex = await texture('orient', [[RED, GREEN], [BLUE, WHITE]]);
  const caps = extra => [{name: 'matter.asset.core', version: 1}, {name: 'matter.material.pbr', version: 1}, ...extra];
  const withTexture = over => ({asset: doc({resources: [tex.descriptor], textures: [tex.texture],
    materials: [material('m', {texture: 'orient'})], ...over}), resources: [tex.bytes]});
  const noRigs = doc(); delete noRigs.rigs;
  const nan = quad('q', 'm'); nan.positions = [[0, 0, 0], [1, 0, 0], [1, NaN, 0], [0, 1, 0]];
  const cases = [
    ['invalid_input', 'alpha below 1', {asset: doc({materials: [material('m', {factor: [1, 1, 1, 0.5]})]})}],
    ['invalid_input', 'undeclared LOD product', {asset: doc({lods: [{prototype: 'q', max_error_metres: 0.01}]})}],
    ['invalid_input', 'singular transform', {asset: doc({instances: [place('i0', 'q', [1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1])]})}],
    ['invalid_input', 'missing product array', {asset: noRigs}],
    ['invalid_input', 'nonfinite position', {asset: doc({meshes: [nan]})}],
    ['invalid_input', 'null source', null],
    ['invalid_input', 'non-asset result', {value: {kind: 'recipe'}}],
    ['invalid_input', 'duplicate capability', {asset: doc({required_capabilities: caps([{name: 'matter.asset.core', version: 1}])})}],
    ['unsupported_version', 'schema major 2', {asset: doc({schema_version: {major: 2, minor: 0}})}],
    ['unsupported_version', 'canonical profile', {asset: doc({canonical_profile: 'matter-exact-json-v2'})}],
    ['unsupported_capability', 'unknown capability', {asset: doc({required_capabilities: caps([{name: 'matter.asset.hair', version: 1}])})}],
    ['unsupported_capability', 'terrain', {asset: doc({required_capabilities: caps([{name: 'matter.asset.terrain', version: 1}])})}],
    ['unsupported_capability', 'required extension', {asset: doc({extensions: [{capability: {name: 'demo.sparkle', version: 1}, required: true, payload: {}}]})}],
    ['unsupported_capability', 'UV set 4', withTexture({materials: [material('m', {texture: 'orient', uvSet: 4})]})],
    ['unsupported_capability', 'core version 2', {asset: doc({required_capabilities: [{name: 'matter.asset.core', version: 2}, {name: 'matter.material.pbr', version: 1}]})}],
    ['unsupported_capability', 'LOD fallback rejected', {asset: doc({required_capabilities: caps([{name: 'matter.asset.lod', version: 1}]),
      lods: [{prototype: 'q', max_error_metres: 0.01}]})}, {fallbacks: 'reject'}],
    ['missing_dependency', 'unknown submesh material', {asset: doc({meshes: [quad('q', 'nope')]})}],
    ['missing_dependency', 'missing texture bytes', {asset: withTexture({}).asset}],
    ['cycle', 'instance parent cycle', {asset: doc({instances: [place('a', 'q', I4, 'mesh', 'b'), place('b', 'q', I4, 'mesh', 'a')]})}],
    ['cycle', 'assembly cycle', {asset: doc({assemblies: [{id: 'A', instances: [place('x', 'B', I4, 'assembly')]},
      {id: 'B', instances: [place('y', 'A', I4, 'assembly')]}], instances: [place('i0', 'A', I4, 'assembly')]})}],
    ['resource_limit', 'instance limit', {asset: doc({instances: [place('a', 'q'), place('b', 'q', T(2, 0, 0))]})}, {maxInstances: 1}],
    ['resource_limit', 'texture size limit', withTexture({}), {maxTextureSize: 1}],
    ['resource_limit', 'assembly depth limit', {asset: doc({assemblies: [{id: 'inner', instances: [place('x', 'q')]},
      {id: 'outer', instances: [place('y', 'inner', I4, 'assembly')]}], instances: [place('i0', 'outer', I4, 'assembly')]})}, {maxDepth: 1}],
  ];
  const calls0 = {...disposeCalls}, gl0 = glCounts();
  const results = [];
  for (const [code, name, source, options] of cases) {
    let error = null;
    try { createAssetView(source, options); } catch (e) { error = e; }
    check(error instanceof ViewerError, `${name}: expected ${code}, got ${error?.stack ?? 'success'}`);
    check(error.code === code, `${name}: expected ${code}, got ${error.code} (${error.message})`);
    const final = error.diagnostics.at(-1);
    check(final?.severity === 'error' && final.code === code, `${name}: last diagnostic ${JSON.stringify(final)}`);
    results.push({case: name, code, message: error.message});
  }
  // Every failure released what it had created.
  const released = {material: disposeCalls.material - calls0.material, texture: disposeCalls.texture - calls0.texture,
    geometry: disposeCalls.geometry - calls0.geometry};
  check(released.material > 0, `failures after material creation released them: ${JSON.stringify(released)}`);
  same(glCounts(), gl0, 'failures allocate no GL objects');
  m.ERRORS = {cases: results, released};
}

// ---------------------------------------------------------------------------
// End to end: real worker runtime, WASM geometry kernel, regeneration session.
async function endToEnd(m, passed) {
  const runtime = new ProceduralRuntime({geometryModuleURL: new URL('/wasm/matter_geometry_wasm.mjs', location.href),
    concurrency: 2, maxQueued: 16, timeoutMs: 20000});
  const session = new DemoSession(runtime, scene, {baseURL: new URL('/', location.href), timeoutMs: 20000});
  setLights(ambient(), directional(1, 2, 3));
  frameView(0, 0.5, 4);
  const gen = async (id, parameters = {}, seed = 1n, viewOptions) => {
    const f = fixture(id);
    return session.generate({fixture: f, parameters: {...defaults(f), ...parameters}, seed, viewOptions});
  };
  const ok = (outcome, label) => {
    check(outcome.status === 'ok', `${label}: ${JSON.stringify(outcome.error ?? outcome.status)}`);
    scene.updateMatrixWorld(true);
    render();
    return outcome.view;
  };
  const firstMaterial = v => [...v.instances.values()][0].object.material;
  try {
    // WEB-VIEW-PROP: texture made in the worker; seed and parameters regenerate.
    // The textured prism is found by material, not by instance order.
    const painted = v => [...v.instances.values()].find(i => i.object.material?.name === 'painted')?.object;
    const prop = ok(await gen('WEB-VIEW-PROP', {}, 7n), 'PROP');
    const map = painted(prop)?.material.map;
    const propMaterials = prop.asset.materials.map(x => x.id);
    check(prop.stats.prototypes === 2 && prop.stats.textures === 1 && prop.stats.materials === propMaterials.length && map?.colorSpace === THREE.SRGBColorSpace,
      `PROP prism and plinth ${JSON.stringify(prop.stats)} declared ${JSON.stringify(propMaterials)}`);
    // The texture must actually be mapped: a UV range of at least one repeat,
    // and adjacent checks on the camera-facing side drawn in different colors.
    const uvRange = object => {
      const uv = object.geometry.getAttribute('uv');
      if (!uv) return null;
      const min = [Infinity, Infinity], max = [-Infinity, -Infinity];
      for (let i = 0; i < uv.count; ++i) for (const [k, x] of [uv.getX(i), uv.getY(i)].entries()) { min[k] = Math.min(min[k], x); max[k] = Math.max(max[k], x); }
      return {min, max};
    };
    const prismUV = uvRange(painted(prop));
    check(prismUV && prismUV.max[0] - prismUV.min[0] >= 1 && prismUV.max[1] - prismUV.min[1] >= 1, `PROP UV range ${JSON.stringify(prismUV)}`);
    const plinth = [...prop.instances.values()].find(i => i.object.material?.name === 'stone')?.object;
    check(plinth, 'PROP extruded plinth drawn with its own material');
    // Default prism: 6 sides, radius 0.8, height 1.2 and 8 checks. The side facing
    // +Z spans x -0.4..0.4 with u 0..1, and v = y * 1.5 / 1.2, so checks are 0.1 m.
    frameView(0, 0.2, 0.5);
    render();
    const [even, odd] = [0, 4].map(k => Array.from(map.image.data.slice(k, k + 3)));
    check(!close(even, odd, 24), `seed 7 checker colors differ: ${even} ${odd}`);
    const [p00, p01, p11, p10] = [[-0.35, 0.05], [-0.25, 0.05], [-0.25, 0.15], [-0.35, 0.15]].map(([x, y]) => pixel(x, y));
    check(close(p00, p11, 6) && close(p01, p10, 6) && !close(p00, p01, 12), `PROP checker drawn: ${JSON.stringify([p00, p01, p11, p10])}`);
    frameView(0, 0.5, 4);
    const texels = Array.from(map.image.data), positions = Array.from(painted(prop).geometry.getAttribute('position').array);
    const again = ok(await gen('WEB-VIEW-PROP', {}, 7n), 'PROP repeat');
    check(prop.disposed && prop.root.parent === null && again.root.parent === scene, 'previous view released on swap');
    same(Array.from(painted(again).material.map.image.data), texels, 'same seed, same texels');
    same(Array.from(painted(again).geometry.getAttribute('position').array), positions, 'same parameters, same positions');
    const reseeded = ok(await gen('WEB-VIEW-PROP', {}, 8n), 'PROP seed 8');
    check(JSON.stringify(Array.from(painted(reseeded).material.map.image.data)) !== JSON.stringify(texels), 'new seed changes the texture');
    const eight = ok(await gen('WEB-VIEW-PROP', {sides: 8}, 8n), 'PROP sides 8');
    const count = v => painted(v).geometry.getAttribute('position').count;
    check(count(eight) !== count(reseeded), 'sides parameter changes the mesh');
    m['E2E-PROP'] = {stats: eight.stats, materials: propMaterials, vertices: {six: count(reseeded), eight: count(eight)},
      uv: {prism: prismUV, plinth: uvRange(plinth)}, checker: {texels: {even, odd}, pixels: [p00, p01, p11, p10]}};
    passed.push('E2E-PROP');

    // WEB-VIEW-CSG: CSG output carries the node material.
    const csg = ok(await gen('WEB-VIEW-CSG'), 'CSG');
    near(firstMaterial(csg).color.r, 0.75, 1e-6, 'CSG clay red (linear factor)');
    check(csg.stats.triangles > 12, `CSG triangles ${csg.stats.triangles}`);
    m['E2E-CSG'] = {stats: csg.stats};
    passed.push('E2E-CSG');

    // WEB-VIEW-ASSEMBLY: shared prototypes, mirrored and sheared placements.
    const assembly = ok(await gen('WEB-VIEW-ASSEMBLY'), 'ASSEMBLY');
    const s = assembly.stats;
    check(s.prototypes === 2 && s.geometries === 2 && s.instances === 16, `ASSEMBLY sharing ${JSON.stringify(s)}`);
    same(s.batches, {instanced: 2, mirroredInstanced: 1, individual: 3}, 'ASSEMBLY batches');
    checkMatrices(assembly, assembly.asset, 'E2E-ASSEMBLY');
    m['E2E-ASSEMBLY'] = {stats: s};
    passed.push('E2E-ASSEMBLY');

    // Repeated updates: GPU objects stay bounded and return to the same count per configuration.
    const configs = [['WEB-VIEW-PROP', {sides: 5}], ['WEB-VIEW-ASSEMBLY', {count: 9}], ['WEB-VIEW-CSG', {segments: 12}]];
    const perConfig = new Map(), sizes = [];
    for (let i = 0; i < 25; ++i) {
      const [id, parameters] = configs[i % configs.length];
      ok(await gen(id, parameters, BigInt(i % configs.length)), `repeat ${i}`);
      const counts = glCounts();
      sizes.push(counts);
      if (perConfig.has(id)) same(counts, perConfig.get(id), `repeat ${i} (${id}) GL objects`);
      else perConfig.set(id, counts);
    }
    m['E2E-REPEAT'] = {iterations: 25, perConfiguration: Object.fromEntries(perConfig), info: {...renderer.info.memory}};
    passed.push('E2E-REPEAT');

    // Supersession: only the newest of a burst lands.
    const burst = await Promise.all(Array.from({length: 8}, (_, i) => gen('WEB-VIEW-PROP', {sides: 3 + i}, BigInt(i))));
    check(burst.slice(0, 7).every(o => o.status === 'superseded') && burst[7].status === 'ok',
      `burst statuses ${burst.map(o => o.status)}`);
    check(session.view === burst[7].view && count(session.view) === count(ok(burst[7], 'burst')), 'newest result is shown');
    m['E2E-SUPERSEDE'] = {statuses: burst.map(o => o.status)};
    passed.push('E2E-SUPERSEDE');

    // Errors keep the last good view.
    const good = ok(await gen('WEB-VIEW-PROP'), 'PROP before errors');
    const errors = {};
    for (const f of FIXTURES.filter(x => x.expectError)) {
      const outcome = await gen(f.id);
      check(outcome.status === 'error' && outcome.error.stage === f.expectError.stage && outcome.error.code === f.expectError.code,
        `${f.id}: ${JSON.stringify(outcome.error ?? outcome.status)}`);
      check(session.view === good && !good.disposed && good.root.parent === scene, `${f.id} must keep the last good view`);
      errors[f.id] = {stage: outcome.error.stage, code: outcome.error.code, message: outcome.error.message};
    }
    m['E2E-ERROR-FIXTURES'] = errors;
    passed.push('E2E-ERROR-KEEPS', 'E2E-ERROR-FIXTURES');

    // Contract fixture: nested assembly, attachment anchor, labeled fallbacks.
    const contract = ok(await gen('WEB-VIEW-CONTRACT'), 'CONTRACT');
    const anchor = contract.attachments.get('socket.top');
    const p = new THREE.Vector3().applyMatrix4(anchor.matrixWorld);
    near(p.x, 3, 1e-9, 'anchor x'); near(p.y, 5, 1e-9, 'anchor y'); near(p.z, 1, 1e-9, 'anchor z');
    const diag = contract.diagnostics;
    for (const capability of ['matter.asset.lod@1', 'matter.asset.collision@1'])
      check(diag.some(d => d.severity === 'warning' && d.code === 'fallback' && d.capability === capability), `fallback label for ${capability}`);
    check(diag.some(d => d.severity === 'info' && d.code === 'ignored' && d.capability === 'demo.sparkle@1'), 'optional extension labeled ignored');
    const rejected = await gen('WEB-VIEW-CONTRACT', {}, 1n, {fallbacks: 'reject'});
    check(rejected.status === 'error' && rejected.error.stage === 'adapter' && rejected.error.code === 'unsupported_capability',
      `reject policy: ${JSON.stringify(rejected.error ?? rejected.status)}`);
    check(session.view === contract, 'rejected fallback keeps the contract view');
    m['E2E-CONTRACT'] = {anchor: [p.x, p.y, p.z], diagnostics: diag, rejected: rejected.error.message};
    passed.push('E2E-CONTRACT');

    session.dispose();
    render();
    same(glCounts(), baseline, 'GL objects after session dispose');
    const after = await gen('WEB-VIEW-PROP');
    check(after.status === 'error' && after.error.stage === 'input' && after.error.code === 'disposed', 'generate after dispose');
    passed.push('E2E-DISPOSED');
    m.runtime = {...runtime.stats};
  } finally {
    session.dispose();
    runtime.dispose();
  }
}

export async function runTests() {
  const passed = [], measurements = {};
  setupRenderer();
  const fixtures = [['COLOR-SRGB', colorSpaceFixture], ['TEX-ORIENT', orientationFixture], ['UVSET1', uvSetFixture],
    ['WINDING', windingFixture], ['COORDS', coordinatesFixture], ['SHEAR', shearFixture], ['TANGENT-MIRROR', tangentFixture],
    ['DISPOSE', disposeFixture], ['ERRORS', errorFixture]];
  for (const [id, run] of fixtures) {
    try { await run(measurements); } catch (error) { throw new Error(`${id}: ${error.stack ?? error}`); }
    render();
    same(glCounts(), baseline, `${id} left GL objects behind`);
    passed.push(id);
  }
  await endToEnd(measurements, passed);
  check(CAPABILITY_MATRIX.length === 8 && NATIVE_ONLY_FEATURES.length === 8, 'capability and native-only tables');
  measurements.environment = {three: THREE.REVISION, renderer: gl.getParameter(gl.VERSION),
    unmaskedRenderer: (() => { const ext = gl.getExtension('WEBGL_debug_renderer_info'); return ext ? gl.getParameter(ext.UNMASKED_RENDERER_WEBGL) : null; })()};
  renderer.dispose();
  return {passed, measurements};
}
