// Browser half of the native/browser matched view (browser_matched.py). Runs
// the unmodified parity generators through the worker runtime, builds the
// viewer's AssetView, and rasterizes it with the native authoring e2e camera
// into a float target. Only the shading is replaced: per-pass ShaderMaterials
// read the viewer's own geometry, instance matrices and material colors, and
// write the debug buffers VulkanRaster produces (linear albedo, world normal,
// reversed depth, instance identity), top-down rows like the native dump.
//
// SwiftShader snaps vertices to 1/16 px (GL SUBPIXEL_BITS 4); native llvmpipe
// to 1/256 px (subPixelPrecisionBits 8). On grazing facets that 1/16-px snap
// alone moves depth by ~1e-4, so the page renders at an odd `scale` and reads
// only each output pixel's centre sample: same sample positions, 1/(16*scale)
// px vertex precision. scale=1 reproduces the plain 128x128 render.
import * as THREE from 'three';
import {createAssetView} from '/integrations/web-procedural/viewer/dist/index.mjs';
import {ProceduralRuntime} from '/integrations/web-procedural/runtime/dist/index.mjs';

const SIZE = 128;
const GENERATORS_URL = new URL('/tests/component-separation/parity/authoring/generators.mjs', location.href).href;
// The parity page's wrapper, with the native e2e's seed.
const authorSource = name => `import * as G from ${JSON.stringify(GENERATORS_URL)};
export default ({sdk, parameters, seed, services}) =>
  G[${JSON.stringify(name)}](sdk, {parameters, seed, services, supportedCapabilities: sdk.DEFAULT_CAPABILITIES});`;

const vertexShader = `
varying vec3 vNormal;
varying float vViewZ;
flat varying int vInstance;
void main() {
  mat4 model = modelMatrix;
#ifdef USE_INSTANCING
  model = model * instanceMatrix;
  vInstance = gl_InstanceID;
#else
  vInstance = 0;
#endif
  vNormal = transpose(inverse(mat3(model))) * normal;
  vec4 view = viewMatrix * model * vec4(position, 1.0);
  vViewZ = view.z;
  gl_Position = projectionMatrix * view;
}`;
const fragmentShader = `
uniform int uMode;
uniform vec3 uColor;
uniform float uBase;
varying vec3 vNormal;
varying float vViewZ;
flat varying int vInstance;
void main() {
  if (uMode == 0) gl_FragColor = vec4(uColor, 1.0);
  else if (uMode == 1) gl_FragColor = vec4(normalize(vNormal), 1.0);
  // Native reversed depth: projection row 2 is (0, 0, 0.1, 0.8).
  else gl_FragColor = vec4(0.1 * vViewZ + 0.8, uBase + float(vInstance), 0.0, 1.0);
}`;

export async function run(generators, scale = 17) {
  if (scale < 1 || scale % 2 !== 1) throw new Error(`scale ${scale} must be odd`);
  const RENDER = SIZE * scale, centre = (scale - 1) / 2;
  const runtime = new ProceduralRuntime({geometryModuleURL: new URL('/wasm/matter_geometry_wasm.mjs', location.href),
    concurrency: 1, maxQueued: 4});
  const renderer = new THREE.WebGLRenderer({antialias: false});
  renderer.setPixelRatio(1);
  renderer.setSize(RENDER, RENDER, false);
  renderer.setClearColor(0x000000, 0);
  const target = new THREE.WebGLRenderTarget(RENDER, RENDER, {type: THREE.FloatType, depthBuffer: true});
  // The authoring e2e camera (identity view): x [-2,4] and y [-1,6] as in
  // native; z keeps native's ordering in GL's [-1,1] (near z=2, far z=-8).
  const camera = new THREE.Camera();
  camera.projectionMatrix.set(1 / 3, 0, 0, -1 / 3, 0, 2 / 7, 0, -5 / 7, 0, 0, -0.2, -0.6, 0, 0, 0, 1);
  camera.projectionMatrixInverse.copy(camera.projectionMatrix).invert();
  const gl = renderer.getContext();
  const out = {environment: {renderer: gl.getParameter(gl.VERSION), subpixel_bits: gl.getParameter(gl.SUBPIXEL_BITS),
    scale, three: THREE.REVISION}};
  try {
    for (const generator of generators) {
      const result = await runtime.submit({kind: 'author', snapshot: {source: authorSource(generator), uri: `fixture://matched/${generator}`},
        parameters: {}, seed: 0x0123456789abcdefn, job: {request_id: `matched.${generator}`, source_revision: '1', generation_epoch: 1n}});
      const view = createAssetView(result, {});
      const scene = new THREE.Scene();
      scene.add(view.root);
      scene.updateMatrixWorld(true);
      // Identity code = per-draw-object base (a slot range per object) + instance index.
      const bases = new Map(), codes = [];
      for (const placed of view.instances.values()) {
        if (!bases.has(placed.object)) bases.set(placed.object, view.instances.size * bases.size);
        codes[bases.get(placed.object) + (placed.instanceIndex ?? 0)] = placed.path;
      }
      const shaded = new Map();
      for (const [object, base] of bases) {
        const materials = Array.isArray(object.material) ? object.material : [object.material];
        if (materials.some(m => m.map)) throw new Error(`${generator}: textured material`);
        const replaced = materials.map(m => new THREE.ShaderMaterial({vertexShader, fragmentShader,
          uniforms: {uMode: {value: 0}, uColor: {value: m.color.clone()}, uBase: {value: base}}}));
        shaded.set(object, {original: object.material, replaced});
        object.material = Array.isArray(object.material) ? replaced : replaced[0];
      }
      const pass = mode => {
        for (const {replaced} of shaded.values()) for (const m of replaced) m.uniforms.uMode.value = mode;
        renderer.setRenderTarget(target);
        renderer.clear();
        renderer.render(scene, camera);
        // Top-down output row y is render row scale*y + centre, i.e. GL
        // (bottom-up) row RENDER-1-(scale*y+centre); keep its centre columns.
        const line = new Float32Array(RENDER * 4), rows = new Float32Array(SIZE * SIZE * 4);
        for (let y = 0; y < SIZE; ++y) {
          renderer.readRenderTargetPixels(target, 0, RENDER - 1 - (scale * y + centre), RENDER, 1, line);
          for (let x = 0; x < SIZE; ++x) rows.set(line.subarray((scale * x + centre) * 4, (scale * x + centre + 1) * 4), (y * SIZE + x) * 4);
        }
        renderer.setRenderTarget(null);
        return rows;
      };
      const albedo = pass(0), normal = pass(1), ident = pass(2);
      const record = {generator, width: SIZE, height: SIZE, instances: [...view.instances.keys()],
        stats: view.stats, identity: [], albedo: [], normal: [], depth: []};
      for (let i = 0; i < SIZE * SIZE; ++i) {
        const covered = ident[4 * i + 3] > 0;
        record.identity.push(covered ? codes[Math.round(ident[4 * i + 1])] ?? `?${ident[4 * i + 1]}` : null);
        record.albedo.push(albedo[4 * i], albedo[4 * i + 1], albedo[4 * i + 2]);
        record.normal.push(normal[4 * i], normal[4 * i + 1], normal[4 * i + 2]);
        record.depth.push(ident[4 * i]);
      }
      // World-space triangles (position + shading normal per corner) of every
      // placed instance, so the compare can ray-cast exact pixel-centre values.
      record.triangles = [];
      for (const placed of view.instances.values()) {
        const object = placed.object, geometry = object.geometry;
        const world = object.matrixWorld.clone();
        if (placed.instanceIndex !== null) {
          const m = new THREE.Matrix4();
          object.getMatrixAt(placed.instanceIndex, m);
          world.multiply(m);
        }
        const normalMatrix = new THREE.Matrix3().getNormalMatrix(world);
        const pos = geometry.getAttribute('position'), nrm = geometry.getAttribute('normal');
        const index = geometry.getIndex();
        const count = index ? index.count : pos.count;
        const p = new THREE.Vector3(), n = new THREE.Vector3(), corners = [];
        for (let k = 0; k < count; ++k) {
          const v = index ? index.getX(k) : k;
          p.fromBufferAttribute(pos, v).applyMatrix4(world);
          n.fromBufferAttribute(nrm, v).applyMatrix3(normalMatrix);
          corners.push(p.x, p.y, p.z, n.x, n.y, n.z);
        }
        record.triangles.push({path: placed.path, corners});
      }
      out[generator] = record;
      for (const [object, {original, replaced}] of shaded) { object.material = original; replaced.forEach(m => m.dispose()); }
      view.dispose();
    }
  } finally {
    runtime.dispose?.();
    target.dispose();
    renderer.dispose();
  }
  return out;
}
