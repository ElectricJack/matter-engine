import type {RecipeDocument, RecipeBuilder, Material, Matrix} from '../../packages/matter-authoring/index.mjs';
export type {RecipeDocument, Material, Matrix};
export type RecipeInput = RecipeDocument | RecipeBuilder | string;
export interface Diagnostic {
  code: string; message: string; severity?: string; node_id?: string;
  source?: {uri: string; line: number; column: number} | null;
  [key: string]: unknown;
}
export class ProceduralError extends Error {
  readonly code: string;
  readonly diagnostics: readonly Diagnostic[];
}
export interface Bounds {min: [number, number, number]; max: [number, number, number]}
export interface MeshBuffers {
  /** Stable prototype ID (recipe node ID). Coordinates are local metres. */
  id: string;
  positions: Float32Array; normals: Float32Array; uvs: Float32Array;
  indices: Uint32Array;
  /** One material-table index per triangle. */
  materialIds: Uint32Array;
  /** One part-table index per triangle; identifies the prototype, not placement. */
  partIds: Uint32Array;
  groups: {start: number; count: number; materialIndex: number}[];
  bounds: Bounds;
}
export interface Instance {
  /** Stable placement ID. Render each instance with its referenced mesh. */
  id: string; partId: string; meshId: string;
  /** Row-major affine matrix; right-handed, Y up. */
  transform: Matrix;
}
export interface MeshLevel {meshes: MeshBuffers[]; instances: Instance[]; bounds: Bounds}
export interface EvaluationResult extends MeshLevel {
  materials: Material[]; parts: string[];
  /** Independent simplifications of the post-modifier base, in requested order. */
  lods: (MeshLevel & {ratio: number})[];
  diagnostics: Diagnostic[];
  measurements: {elapsedMs: number; wasmHeapBytes: number; transferredBytes: number};
}
export type Modifier = {operation: 'mesh.simplify'; ratio: number}
  | {operation: 'mesh.smooth'; iterations?: number; lambda?: number; mu?: number};
export interface Budget {
  operations?: number; vertices?: number; indices?: number;
  output_bytes?: number; working_bytes?: number; execution_microseconds?: number;
  dependency_depth?: number; progress_events?: number;
}
export interface EvaluateOptions {
  signal?: AbortSignal;
  /** Execution timeout, 1..60000ms, excluding time queued. Default 10000. */
  timeoutMs?: number;
  budget?: Budget;
  /** Ordered stack applied to all prototypes. */
  modifiers?: readonly Modifier[];
  /** Up to eight ratios in (0,1); base geometry is always returned separately. */
  lodRatios?: readonly number[];
}
export interface EngineOptions {
  /** Defaults to the package's adjacent worker.mjs and matter_geometry_wasm.mjs. */
  workerURL?: string | URL; moduleURL?: string | URL;
  /** 1..8, default 1. */
  concurrency?: number;
  /** 0..1024, default 32. */
  maxQueued?: number;
  timeoutMs?: number;
}
export interface ProceduralEngine {
  evaluate(recipe: RecipeInput, options?: EvaluateOptions): Promise<EvaluationResult>;
  /** Cancels pending jobs, terminates workers. Idempotent. */
  dispose(): void;
}
/** Browser module Workers or Node worker_threads; initialization is lazy per worker. */
export function createProceduralEngine(options?: EngineOptions): Promise<ProceduralEngine>;
/** Deduplicated, JS-owned buffers safe to pass as a postMessage transfer list. */
export function transferables(result: EvaluationResult): ArrayBuffer[];
