import {BufferAttribute, BufferGeometry, Group, InstancedMesh, Matrix4, Mesh, Object3D, type Material} from 'three';
import {checkCapabilities, type FallbackPolicy} from './capabilities.mjs';
import {DiagnosticLog, ViewerError, type ViewerDiagnostic} from './errors.mjs';
import {MaterialLibrary, collectResourceBytes, MAX_UV_SET, type MaterialOptions} from './materials.mjs';
import {IDENTITY, MIRROR_X, checkAffine, classify, isMirror, multiply, type Affine, type PlacementClass} from './matrix.mjs';

export const DEFAULT_MAX_INSTANCES = 100000;
export const DEFAULT_MAX_DEPTH = 32;

/** Typed vertex data as returned by the geometry binding / web runtime. */
export interface MeshBuffers {
  readonly id: string;
  readonly positions: Float32Array;
  readonly normals: Float32Array;
  readonly tangents: Float32Array;
  readonly uv_sets: readonly Float32Array[];
  readonly indices: Uint32Array;
}

/** A runtime/binding result (`{value:{kind:'asset', asset, resources}, meshes}`)
 *  or a bare contract asset document with optional typed buffers and resource bytes. */
export type AssetSource =
  | {readonly value: {readonly kind: string; readonly asset?: unknown; readonly resources?: unknown}; readonly meshes?: readonly MeshBuffers[]}
  | {readonly asset: unknown; readonly meshes?: readonly MeshBuffers[]; readonly resources?: unknown};

export interface AssetViewOptions extends MaterialOptions {
  /** `label` (default) draws fallbacks and reports them; `reject` fails instead. */
  readonly fallbacks?: FallbackPolicy;
  /** Upper bound on flattened mesh placements (resource_limit beyond it). */
  readonly maxInstances?: number;
  /** Upper bound on assembly nesting depth (resource_limit beyond it). */
  readonly maxDepth?: number;
}

export interface PlacedInstance {
  /** Scoped instance path, e.g. `placed.b/child.a`. */
  readonly path: string;
  readonly prototype: string;
  /** Asset-space world transform, contract row-major. */
  readonly world: Affine;
  readonly placement: PlacementClass;
  readonly object: Mesh;
  /** Index in `object` when it is an InstancedMesh, else null. */
  readonly instanceIndex: number | null;
}

export interface AssetViewStats {
  /** Mesh prototypes declared by the asset. */
  readonly prototypes: number;
  /** Uploaded geometries: one per placed prototype, plus a tangent-flipped variant when mirrored placements need one. */
  readonly geometries: number;
  readonly materials: number;
  readonly textures: number;
  /** Texture objects including per-UV-channel clones (which share uploaded texels). */
  readonly textureObjects: number;
  readonly instances: number;
  readonly drawObjects: number;
  readonly batches: {readonly instanced: number; readonly mirroredInstanced: number; readonly individual: number};
  /** Triangles drawn per frame across all placements. */
  readonly triangles: number;
  /** Triangles uploaded (each prototype once). */
  readonly uploadedTriangles: number;
}

export interface AssetView {
  /** Group in asset space (metres, right-handed, Y up). Add it to a scene. */
  readonly root: Group;
  readonly asset: Readonly<Record<string, unknown>>;
  readonly diagnostics: readonly ViewerDiagnostic[];
  readonly instances: ReadonlyMap<string, PlacedInstance>;
  /** Non-rendering anchors at instance world * attachment local. */
  readonly attachments: ReadonlyMap<string, Object3D>;
  readonly stats: AssetViewStats;
  readonly disposed: boolean;
  /** Detach from the scene and release every geometry, material and texture. Idempotent. */
  dispose(): void;
}

interface InstanceValue { id: string; prototype: string; target: string; parent: string | null; local_transform: number[] }
interface SubmeshValue { first_index: number; index_count: number; material: string }
interface MeshValue {
  id: string; positions: unknown[]; normals: unknown[]; tangents: unknown[]; uv_sets: unknown[][]; indices: unknown[];
  submeshes: SubmeshValue[]; winding: string;
}

interface ScopeEntry { value: InstanceValue; chain: Affine }
interface Placement { path: string; prototype: string; world: Affine; placement: PlacementClass }

const ARRAY_FIELDS = ['required_capabilities', 'resources', 'materials', 'textures', 'extensions', 'meshes', 'assemblies',
  'instances', 'attachments', 'lods', 'collisions', 'rigs', 'skins', 'animations'] as const;

const isObject = (value: unknown): value is Record<string, unknown> => typeof value === 'object' && value !== null && !Array.isArray(value);
const uvName = (set: number) => (set === 0 ? 'uv' : `uv${set}`);

function setMatrix(target: Matrix4, m: Affine): Matrix4 {
  return target.set(m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
}

function unpack(rows: unknown, width: number, path: string, what: string, log: DiagnosticLog): Float32Array {
  if (!Array.isArray(rows)) log.fail('invalid_input', `${what} must be an array`, {path});
  const out = new Float32Array(rows.length * width);
  rows.forEach((row: unknown, i: number) => {
    if (!Array.isArray(row) || row.length !== width) log.fail('invalid_input', `${what}[${i}] must have ${width} numbers`, {path});
    for (let k = 0; k < width; ++k) {
      if (typeof row[k] !== 'number') log.fail('invalid_input', `${what}[${i}] must hold numbers`, {path});
      out[i * width + k] = row[k] as number;
    }
  });
  return out;
}

function allFinite(values: Float32Array): boolean {
  for (let i = 0; i < values.length; ++i) if (!Number.isFinite(values[i])) return false;
  return true;
}

/** Validated vertex data for one prototype, typed and ready to upload. */
interface PrototypeData {
  id: string; vertexCount: number; positions: Float32Array; normals: Float32Array | null; tangents: Float32Array | null;
  uvSets: Float32Array[]; indices: Uint32Array; submeshes: SubmeshValue[];
}

function readMesh(mesh: MeshValue, buffers: MeshBuffers | undefined, materials: MaterialLibrary, log: DiagnosticLog): PrototypeData {
  const path = `meshes/${mesh.id}`;
  if (mesh.winding !== 'ccw') log.fail('invalid_input', 'v1 mesh winding must be ccw', {path});
  for (const field of ['positions', 'normals', 'tangents', 'uv_sets', 'indices', 'submeshes'] as const)
    if (!Array.isArray(mesh[field])) log.fail('invalid_input', `${field} must be an array`, {path});
  const positions = buffers ? buffers.positions : unpack(mesh.positions, 3, path, 'positions', log);
  const vertexCount = positions.length / 3;
  if (!Number.isInteger(vertexCount) || vertexCount < 1) log.fail('invalid_input', 'mesh needs at least one position', {path});
  // Binding results carry both: typed buffers and the document's arrays must agree.
  if (buffers && mesh.positions.length !== 0 && mesh.positions.length !== vertexCount)
    log.fail('invalid_input', 'typed positions disagree with the document', {path});
  const sized = (data: Float32Array | null, width: number, what: string) => {
    if (data && data.length !== 0 && data.length !== vertexCount * width)
      log.fail('invalid_input', `${what} must be empty or one per position`, {path});
    return data && data.length !== 0 ? data : null;
  };
  const normals = sized(buffers ? buffers.normals : unpack(mesh.normals, 3, path, 'normals', log), 3, 'normals');
  const tangents = sized(buffers ? buffers.tangents : unpack(mesh.tangents, 4, path, 'tangents', log), 4, 'tangents');
  const uvRows = buffers ? buffers.uv_sets : mesh.uv_sets.map((set, i) => unpack(set, 2, path, `uv_sets[${i}]`, log));
  if (buffers && mesh.uv_sets.length !== 0 && mesh.uv_sets.length !== uvRows.length)
    log.fail('invalid_input', 'typed UV sets disagree with the document', {path});
  const uvSets = uvRows.map((set, i) => {
    if (set.length !== vertexCount * 2) log.fail('invalid_input', `uv_sets[${i}] must have one UV per position`, {path});
    return set;
  });
  for (const [data, what] of [[positions, 'positions'], [normals, 'normals'], [tangents, 'tangents'], ...uvSets.map((s, i) => [s, `uv_sets[${i}]`])] as [Float32Array | null, string][])
    if (data && !allFinite(data)) log.fail('invalid_input', `nonfinite ${what}`, {path});
  const bounds = (mesh as {bounds?: unknown}).bounds;
  const lo = isObject(bounds) ? bounds.min : undefined, hi = isObject(bounds) ? bounds.max : undefined;
  if (!Array.isArray(lo) || !Array.isArray(hi) || lo.length !== 3 || hi.length !== 3 ||
      ![0, 1, 2].every(k => Number.isFinite(lo[k]) && Number.isFinite(hi[k]) && lo[k] <= hi[k]))
    log.fail('invalid_input', 'invalid bounds', {path});
  {
    // The document's f64 positions are checked exactly, like the native validator
    // (before f32 rounding, which can step past a bound: f32(0.1) > 0.1); typed
    // f32 buffers get one float ulp of slack around the f64 bounds.
    const rows = buffers ? null : (mesh.positions as number[][]);
    const slack = buffers ? 1e-6 * (1 + Math.max(...lo.map(Math.abs), ...hi.map(Math.abs))) : 0;
    for (let i = 0; i < positions.length; ++i) {
      const k = i % 3, x = rows ? rows[(i - k) / 3][k] : positions[i];
      if (x < lo[k] - slack || x > hi[k] + slack) log.fail('invalid_input', 'position outside mesh bounds', {path});
    }
  }
  if (tangents)
    for (let i = 3; i < tangents.length; i += 4)
      if (tangents[i] !== 1 && tangents[i] !== -1) log.fail('invalid_input', 'tangent handedness (w) must be +1 or -1', {path});

  let indices: Uint32Array;
  if (buffers) {
    indices = buffers.indices;
    if (mesh.indices.length !== 0 && mesh.indices.length !== indices.length)
      log.fail('invalid_input', 'typed indices disagree with the document', {path});
  } else {
    if (!mesh.indices.every(i => Number.isInteger(i) && (i as number) >= 0 && (i as number) <= 0xffffffff))
      log.fail('invalid_input', 'indices must be u32 integers', {path});
    indices = Uint32Array.from(mesh.indices as number[]);
  }
  if (indices.length === 0 || indices.length % 3 !== 0) log.fail('invalid_input', 'mesh indices must be complete triangles', {path});
  for (let i = 0; i < indices.length; ++i)
    if (indices[i] >= vertexCount) log.fail('invalid_input', `index ${indices[i]} outside ${vertexCount} positions`, {path});

  if (mesh.submeshes.length === 0) log.fail('invalid_input', 'mesh needs at least one submesh', {path});
  let end = 0;
  for (const s of mesh.submeshes) {
    if (!isObject(s) || s.first_index !== end || !Number.isInteger(s.index_count) || s.index_count <= 0 || s.index_count % 3 !== 0)
      log.fail('invalid_input', 'submeshes must partition the triangles in order', {path});
    end += s.index_count;
    if (!materials.get(s.material)) log.fail('missing_dependency', `submesh material ${String(s.material)} is not declared`, {path});
    for (const set of materials.uvSetsUsed(s.material))
      if (set >= uvSets.length)
        log.fail('invalid_input', `material ${s.material} samples UV set ${set} but the mesh has ${uvSets.length}`, {path});
  }
  if (end !== indices.length) log.fail('invalid_input', 'submeshes must cover all indices', {path});
  if (uvSets.length > MAX_UV_SET + 1)
    log.note('info', 'ignored', `UV sets beyond ${MAX_UV_SET} are not uploaded (no material can sample them)`, {path});
  return {id: mesh.id, vertexCount, positions, normals, tangents, uvSets, indices, submeshes: mesh.submeshes};
}

/** Build a three.js view of a contract v1 asset. Validation failures throw a
 *  ViewerError carrying the contract error code and every diagnostic so far;
 *  nothing is left allocated after a failure. */
export function createAssetView(source: AssetSource, options: AssetViewOptions = {}): AssetView {
  const log: DiagnosticLog = new DiagnosticLog();
  const geometries: BufferGeometry[] = [];
  const drawObjects: Mesh[] = [];
  let materials: MaterialLibrary | null = null;
  const release = () => {
    for (const object of drawObjects) if (object instanceof InstancedMesh) object.dispose();
    for (const geometry of geometries) geometry.dispose();
    materials?.dispose();
  };
  try {
    return build();
  } catch (error) {
    release();
    if (error instanceof ViewerError) throw error;
    // Anything else is an adapter defect or a hostile input shape; still report a contract code.
    throw new ViewerError('invalid_input', error instanceof Error ? error.message : String(error), log.entries);
  }

  function build(): AssetView {
    if (!isObject(source)) log.fail('invalid_input', 'source must be a geometry result or {asset}');
    let asset: unknown, resourceEntries: unknown;
    if ('value' in source) {
      if (!isObject(source.value) || source.value.kind !== 'asset')
        log.fail('invalid_input', `result kind ${String((source.value as {kind?: unknown})?.kind)} carries no renderable asset`);
      asset = source.value.asset; resourceEntries = source.value.resources;
    } else {
      asset = source.asset; resourceEntries = source.resources;
    }
    if (!isObject(asset)) log.fail('invalid_input', 'asset document must be an object');
    const doc = asset as Record<string, unknown>;
    if (doc.document_type !== 'asset') log.fail('invalid_input', `document_type ${String(doc.document_type)} is not asset`);
    if (doc.canonical_profile !== 'matter-exact-json-v1')
      log.fail('unsupported_version', `canonical_profile ${String(doc.canonical_profile)} is not matter-exact-json-v1`);
    const version = doc.schema_version as {major?: unknown; minor?: unknown} | undefined;
    if (!isObject(version) || version.major !== 1 || version.minor !== 0)
      log.fail('unsupported_version', `schema_version ${JSON.stringify(version)} is not 1.0`);
    for (const field of ARRAY_FIELDS) if (!Array.isArray(doc[field])) log.fail('invalid_input', `${field} must be an array`);
    checkCapabilities(doc, options.fallbacks ?? 'label', log);

    const meshValues = new Map<string, MeshValue>();
    const assemblyValues = new Map<string, InstanceValue[]>();
    for (const m of doc.meshes as MeshValue[]) {
      if (!isObject(m) || typeof m.id !== 'string') log.fail('invalid_input', 'mesh needs an id');
      if (meshValues.has(m.id)) log.fail('invalid_input', 'duplicate prototype id', {path: `meshes/${m.id}`});
      meshValues.set(m.id, m);
    }
    for (const a of doc.assemblies as {id: string; instances: InstanceValue[]}[]) {
      if (!isObject(a) || typeof a.id !== 'string' || !Array.isArray(a.instances)) log.fail('invalid_input', 'assembly needs an id and instances');
      // Meshes and assemblies share one prototype ID scope.
      if (meshValues.has(a.id) || assemblyValues.has(a.id)) log.fail('invalid_input', 'duplicate prototype id', {path: `assemblies/${a.id}`});
      assemblyValues.set(a.id, a.instances);
    }

    const buffers = new Map<string, MeshBuffers>();
    if (source.meshes !== undefined) {
      if (!Array.isArray(source.meshes)) log.fail('invalid_input', 'typed meshes must be an array');
      for (const b of source.meshes as readonly MeshBuffers[]) {
        if (!isObject(b) || typeof b.id !== 'string' || !meshValues.has(b.id)) log.fail('invalid_input', `typed buffers for undeclared mesh ${String((b as {id?: unknown})?.id)}`);
        if (buffers.has(b.id)) log.fail('invalid_input', 'duplicate typed buffers', {path: `meshes/${b.id}`});
        if (!(b.positions instanceof Float32Array) || !(b.normals instanceof Float32Array) || !(b.tangents instanceof Float32Array) ||
            !(b.indices instanceof Uint32Array) || !Array.isArray(b.uv_sets) || !b.uv_sets.every(u => u instanceof Float32Array))
          log.fail('invalid_input', 'typed buffers must be Float32Array/Uint32Array', {path: `meshes/${b.id}`});
        buffers.set(b.id, b);
      }
    }

    materials = new MaterialLibrary(doc, collectResourceBytes(resourceEntries, log), options, log);
    const prototypes = new Map<string, PrototypeData>();
    for (const mesh of meshValues.values()) prototypes.set(mesh.id, readMesh(mesh, buffers.get(mesh.id), materials, log));

    // Validate every scope once: unique IDs, resolvable parents and targets,
    // acyclic parent chains, valid transforms. `chain` is the scope-local world.
    const maxDepth = options.maxDepth ?? DEFAULT_MAX_DEPTH;
    const scope = (instances: InstanceValue[], where: string): Map<string, ScopeEntry> => {
      const byId = new Map<string, InstanceValue>();
      for (const x of instances) {
        if (!isObject(x) || typeof x.id !== 'string') log.fail('invalid_input', 'instance needs an id', {path: where});
        if (byId.has(x.id)) log.fail('invalid_input', 'duplicate instance id in scope', {path: `${where}${x.id}`});
        byId.set(x.id, x);
      }
      const out = new Map<string, ScopeEntry>();
      const visiting = new Set<string>();
      const resolve = (x: InstanceValue): Affine => {
        const done = out.get(x.id);
        if (done) return done.chain;
        const path = `${where}${x.id}`;
        if (visiting.has(x.id)) log.fail('cycle', 'cyclic instance parent chain', {path});
        visiting.add(x.id);
        const local = checkAffine(x.local_transform, path, log);
        let chain = local;
        if (x.parent !== null) {
          const parent = byId.get(x.parent);
          if (!parent) log.fail('missing_dependency', `parent ${String(x.parent)} is not in the same scope`, {path});
          chain = multiply(resolve(parent), local);  // parent * local: local applies first
        }
        if (x.target === 'mesh') {
          if (!meshValues.has(x.prototype)) log.fail('missing_dependency', `instance mesh ${String(x.prototype)} is not declared`, {path});
        } else if (x.target === 'assembly') {
          if (!assemblyValues.has(x.prototype)) log.fail('missing_dependency', `instance assembly ${String(x.prototype)} is not declared`, {path});
        } else log.fail('invalid_input', `invalid instance target ${String(x.target)}`, {path});
        visiting.delete(x.id);
        out.set(x.id, {value: x, chain});
        return chain;
      };
      for (const x of instances) resolve(x);
      return out;
    };
    const assemblyScopes = new Map<string, Map<string, ScopeEntry>>();
    for (const [id, instances] of assemblyValues) assemblyScopes.set(id, scope(instances, `assemblies/${id}/`));
    const roots = scope(doc.instances as InstanceValue[], 'instances/');

    // Assembly graph: reject cycles and over-deep nesting before expanding anything.
    const height = new Map<string, number>();
    const onStack = new Set<string>();
    const measure = (id: string): number => {
      const known = height.get(id);
      if (known !== undefined) return known;
      if (onStack.has(id)) log.fail('cycle', 'cyclic assembly reference', {path: `assemblies/${id}`});
      onStack.add(id);
      let h = 1;
      for (const {value} of assemblyScopes.get(id)!.values()) if (value.target === 'assembly') h = Math.max(h, 1 + measure(value.prototype));
      onStack.delete(id);
      if (h > maxDepth) log.fail('resource_limit', `assembly nesting depth exceeds ${maxDepth}`, {path: `assemblies/${id}`});
      height.set(id, h);
      return h;
    };
    for (const id of assemblyValues.keys()) measure(id);

    const maxInstances = options.maxInstances ?? DEFAULT_MAX_INSTANCES;
    const placements: Placement[] = [];
    const expand = (entries: Map<string, ScopeEntry>, parentWorld: Affine, prefix: string) => {
      for (const {value, chain} of entries.values()) {
        const world = parentWorld === IDENTITY ? chain : multiply(parentWorld, chain);
        const path = `${prefix}${value.id}`;
        if (value.target === 'assembly') { expand(assemblyScopes.get(value.prototype)!, world, `${path}/`); continue; }
        if (placements.length >= maxInstances) log.fail('resource_limit', `more than ${maxInstances} mesh placements`, {path});
        placements.push({path, prototype: value.prototype, world, placement: classify(world)});
      }
    };
    expand(roots, IDENTITY, '');

    const root = new Group();
    root.name = String(doc.id);
    root.userData.matter = {asset: doc.id};

    const attachments = new Map<string, Object3D>();
    for (const a of doc.attachments as {id: string; instance: string; local_transform: number[]}[]) {
      const path = `attachments/${isObject(a) ? String(a.id) : '?'}`;
      if (!isObject(a) || typeof a.id !== 'string') log.fail('invalid_input', 'attachment needs an id', {path});
      if (attachments.has(a.id)) log.fail('invalid_input', 'duplicate attachment id', {path});
      const owner = roots.get(a.instance);
      if (!owner) log.fail('missing_dependency', `attachment instance ${String(a.instance)} is not a root instance`, {path});
      const anchor = new Object3D();
      anchor.name = a.id;
      anchor.matrixAutoUpdate = false;
      setMatrix(anchor.matrix, multiply(owner.chain, checkAffine(a.local_transform, path, log)));
      anchor.userData.matter = {attachment: a.id, instance: a.instance};
      attachments.set(a.id, anchor);
      root.add(anchor);
    }
    // LOD and collision products are validated like the native consumer but not
    // drawn (capability fallbacks, labeled by checkCapabilities).
    const lodPrototypes = new Set<string>();
    for (const l of doc.lods as {prototype: string; max_error_metres: number}[]) {
      if (!isObject(l)) log.fail('invalid_input', 'LOD entry must be an object', {path: 'lods'});
      const path = `lods/${String(l.prototype)}`;
      if (!meshValues.has(l.prototype)) log.fail('missing_dependency', `LOD prototype ${String(l.prototype)} is not a mesh`, {path});
      if (lodPrototypes.has(l.prototype)) log.fail('invalid_input', 'duplicate LOD prototype', {path});
      if (!Number.isFinite(l.max_error_metres) || l.max_error_metres < 0) log.fail('invalid_input', 'LOD max_error_metres must be finite and >= 0', {path});
      lodPrototypes.add(l.prototype);
    }
    const collisionIds = new Set<string>(), collisionPrototypes = new Set<string>();
    for (const c of doc.collisions as {id: string; instance: string; shape: string; prototype: string | null; size: number[]}[]) {
      if (!isObject(c) || typeof c.id !== 'string') log.fail('invalid_input', 'collision needs an id', {path: 'collisions'});
      const path = `collisions/${c.id}`;
      if (collisionIds.has(c.id)) log.fail('invalid_input', 'duplicate collision id', {path});
      collisionIds.add(c.id);
      if (!roots.has(c.instance)) log.fail('missing_dependency', `collision instance ${String(c.instance)} is not a root instance`, {path});
      if (!['mesh', 'convex_mesh', 'box', 'capsule'].includes(c.shape)) log.fail('invalid_input', `invalid collision shape ${String(c.shape)}`, {path});
      const meshShape = c.shape === 'mesh' || c.shape === 'convex_mesh';
      if (meshShape) {
        if (c.prototype === null || !meshValues.has(c.prototype)) log.fail('missing_dependency', `collision prototype ${String(c.prototype)} is not a mesh`, {path});
        collisionPrototypes.add(c.prototype);
      } else if (c.prototype !== null) log.fail('invalid_input', 'analytic collision shapes take a null prototype', {path});
      const size = c.size;
      if (!Array.isArray(size) || size.length !== 3 || !size.every(x => Number.isFinite(x) && x > 0))
        log.fail('invalid_input', 'collision size must be three positive numbers', {path});
      if (c.shape === 'capsule' && (size[0] !== size[2] || size[1] < 2 * size[0]))
        log.fail('invalid_input', 'capsule size needs x == z and y >= 2x', {path});
    }

    // One geometry per placed prototype; batches share it.
    const byPrototype = new Map<string, Placement[]>();
    for (const p of placements) {
      const list = byPrototype.get(p.prototype);
      if (list) list.push(p); else byPrototype.set(p.prototype, [p]);
    }
    for (const id of meshValues.keys())
      if (!byPrototype.has(id) && !lodPrototypes.has(id) && !collisionPrototypes.has(id))
        log.note('info', 'unplaced', 'mesh prototype is not placed by any instance and is not drawn', {path: `meshes/${id}`});

    const instances = new Map<string, PlacedInstance>();
    const batches = {instanced: 0, mirroredInstanced: 0, individual: 0};
    let triangles = 0, uploadedTriangles = 0;
    const scratch = new Matrix4();
    for (const [id, list] of byPrototype) {
      const data = prototypes.get(id)!;
      const {base, mirrored, material} = upload(data, list.some(p => p.placement === 'mirrored'), materials, log, geometries);
      uploadedTriangles += data.indices.length / 3;
      triangles += (data.indices.length / 3) * list.length;
      const record = (p: Placement, object: Mesh, instanceIndex: number | null) =>
        instances.set(p.path, Object.freeze({path: p.path, prototype: id, world: Object.freeze([...p.world]), placement: p.placement, object, instanceIndex}));
      const single = (p: Placement) => {
        // A negative determinant flips three.js to clockwise front faces and needs the w-flipped tangents.
        const mesh = new Mesh(isMirror(p.world) ? mirrored : base, material);
        mesh.name = p.path;
        mesh.matrixAutoUpdate = false;
        setMatrix(mesh.matrix, p.world);
        mesh.userData.matter = {prototype: id, placement: p.placement, instances: [p.path]};
        drawObjects.push(mesh); root.add(mesh); record(p, mesh, null);
        batches.individual += 1;
      };
      const batch = (group: Placement[], mirror: boolean) => {
        if (group.length === 0) return;
        if (group.length === 1) { single(group[0]); return; }
        const mesh = new InstancedMesh(mirror ? mirrored : base, material, group.length);
        mesh.name = `${id}${mirror ? ' (mirrored)' : ''} x${group.length}`;
        mesh.matrixAutoUpdate = false;
        // A mirrored batch draws MIRROR_X * (MIRROR_X * T) = T. The object
        // determinant (< 0) is what makes three.js switch to clockwise front faces.
        setMatrix(mesh.matrix, mirror ? MIRROR_X : IDENTITY);
        group.forEach((p, i) => {
          mesh.setMatrixAt(i, setMatrix(scratch, mirror ? multiply(MIRROR_X, p.world) : p.world));
          record(p, mesh, i);
        });
        mesh.instanceMatrix.needsUpdate = true;
        mesh.computeBoundingSphere();
        mesh.userData.matter = {prototype: id, placement: mirror ? 'mirrored' : 'orthogonal', instances: group.map(p => p.path)};
        drawObjects.push(mesh); root.add(mesh);
        if (mirror) batches.mirroredInstanced += 1; else batches.instanced += 1;
      };
      batch(list.filter(p => p.placement === 'orthogonal'), false);
      batch(list.filter(p => p.placement === 'mirrored'), true);
      // Sheared placements: three.js instancing cannot transform their normals.
      for (const p of list.filter(p => p.placement === 'general')) single(p);
    }

    const lib = materials;
    const libStats = lib.stats;
    const stats: AssetViewStats = Object.freeze({
      prototypes: meshValues.size, geometries: geometries.length, materials: libStats.materials, textures: libStats.textures,
      textureObjects: libStats.textureObjects, instances: placements.length, drawObjects: drawObjects.length,
      batches: Object.freeze(batches), triangles, uploadedTriangles,
    });
    const diagnostics = Object.freeze([...log.entries]);
    let disposed = false;
    return Object.freeze({
      root, asset: doc, diagnostics, instances, attachments, stats,
      get disposed() { return disposed; },
      dispose() {
        if (disposed) return;
        disposed = true;
        root.removeFromParent();
        release();
        root.clear();
      },
    });
  }
}

/** Upload one prototype: shared attributes, submesh groups, and (only when a
 *  mirrored placement samples a normal map through the mesh's tangents) a
 *  w-flipped tangent variant, since three.js derives the bitangent as cross(N, T) * w. */
function upload(data: PrototypeData, hasMirrored: boolean, materials: MaterialLibrary, log: DiagnosticLog, geometries: BufferGeometry[]):
    {base: BufferGeometry; mirrored: BufferGeometry; material: Material | Material[]} {
  const path = `meshes/${data.id}`;
  const geometry = new BufferGeometry();
  geometry.name = data.id;
  geometries.push(geometry);
  geometry.setAttribute('position', new BufferAttribute(data.positions, 3));
  if (data.normals) geometry.setAttribute('normal', new BufferAttribute(data.normals, 3));
  const used = new Set<number>();
  let needsTangents = false;
  for (const s of data.submeshes) {
    for (const set of materials.uvSetsUsed(s.material)) used.add(set);
    needsTangents ||= materials.usesNormalMap(s.material);
  }
  for (const set of [...used].sort()) geometry.setAttribute(uvName(set), new BufferAttribute(data.uvSets[set], 2));
  if (needsTangents && data.tangents) geometry.setAttribute('tangent', new BufferAttribute(data.tangents, 4));
  else if (needsTangents)
    log.note('info', 'derived', 'mesh has no tangents; the normal map uses a screen-space derivative tangent frame', {path});
  geometry.setIndex(new BufferAttribute(data.indices, 1));
  if (!data.normals) {
    geometry.computeVertexNormals();
    log.note('info', 'derived', 'mesh has no normals; smooth vertex normals were derived', {path});
  }
  const material = data.submeshes.length === 1 ? materials.get(data.submeshes[0].material)!
    : data.submeshes.map(s => materials.get(s.material)!);
  if (data.submeshes.length > 1) data.submeshes.forEach((s, i) => geometry.addGroup(s.first_index, s.index_count, i));

  let mirrored = geometry;
  const tangent = geometry.getAttribute('tangent');
  if (tangent && hasMirrored) {
    mirrored = new BufferGeometry();
    mirrored.name = `${data.id} (mirrored tangents)`;
    geometries.push(mirrored);
    // Share every GPU buffer except the tangent one.
    for (const [name, attribute] of Object.entries(geometry.attributes))
      if (name !== 'tangent') mirrored.setAttribute(name, attribute);
    const flipped = new Float32Array(tangent.array as Float32Array);
    for (let i = 3; i < flipped.length; i += 4) flipped[i] = -flipped[i];
    mirrored.setAttribute('tangent', new BufferAttribute(flipped, 4));
    mirrored.setIndex(geometry.index!);
    for (const g of geometry.groups) mirrored.addGroup(g.start, g.count, g.materialIndex);
  }
  return {base: geometry, mirrored, material};
}
