export interface Capability { name: string; version: number }
export interface Diagnostic { code?: string; status?: number; message: string; severity?: string; node_id?: string }
export class GeometryError extends Error {
  readonly status: number;
  readonly diagnostics: Diagnostic[];
}
export interface Job {
  request_id: string;
  source_revision: string;
  /** Exact uint64. JSON result echoes sixteen lowercase hexadecimal digits. */
  generation_epoch: bigint | number;
}
export interface Budget {
  operations?: bigint | number; vertices?: bigint | number; indices?: bigint | number;
  output_bytes?: bigint | number; working_bytes?: bigint | number;
  execution_microseconds?: bigint | number; queries?: bigint | number;
  dependency_depth?: number; progress_events?: number;
}
export interface CallOptions { job: Job; budget?: Budget; signal?: AbortSignal }
export interface MeshBuffers {
  id: string; positions: Float32Array; normals: Float32Array; tangents: Float32Array;
  uv_sets: Float32Array[]; indices: Uint32Array;
}
export interface GeometryResult {
  /** Owned copies. Transferable after release; never views of WASM memory. */
  wire: Uint8Array;
  value: {binding_version: 1; kind: 'asset' | 'query'; job: Omit<Job, 'generation_epoch'> & {generation_epoch: string};
    asset?: Record<string, unknown>; resources?: unknown[]; samples?: unknown[]};
  meshes: MeshBuffers[];
}
export interface Geometry {
  capabilities(): Capability[];
  evaluate(request: object | string, options: CallOptions): GeometryResult;
  query(asset: GeometryResult, request: object | string, options: CallOptions): GeometryResult;
  process(asset: GeometryResult, request: object | string, options: CallOptions): GeometryResult;
  release(result: GeometryResult): void;
  dispose(): void;
}
export function createGeometry(factory: (options: Record<string, unknown>) => Promise<unknown>, options?: {
  moduleOptions?: Record<string, unknown>;
  limits?: {max_results?: number; max_retained_bytes?: number; max_request_bytes?: number};
}): Promise<Geometry>;
