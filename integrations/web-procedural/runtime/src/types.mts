import type {Budget, Capability, Diagnostic, GeometryResult, Job} from '../../../../bindings/geometry-c/wasm/geometry.js';
import type * as SDK from '../../../../packages/matter-authoring/index.mjs';
export type {Budget, Capability, Diagnostic, GeometryResult, Job};

export type ErrorCode = 'invalid_input' | 'unsupported_capability' | 'unsupported_version' | 'missing_dependency' | 'cycle' | 'resource_limit' | 'cancelled' | 'stale_result' | 'backend_failure' | 'invalid_handle' | 'disposed';
export interface AuthoringSnapshot {
  /** Frozen, self-contained ES module exporting default(context). SDK is injected. */
  source: string;
  uri: string;
}
export interface JobBase { job: Job; budget?: Budget; maxStages?: number }
export interface EvaluateJob extends JobBase { kind: 'evaluate'; request: object | string }
export interface AuthoringJob extends JobBase {
  kind: 'author'; snapshot: AuthoringSnapshot;
  parameters?: SDK.JsonObject; seed?: SDK.Seed;
}
export type RuntimeJob = EvaluateJob | AuthoringJob;
export interface AuthoringContext {
  readonly sdk: typeof SDK;
  readonly job: Readonly<Job>;
  readonly parameters: SDK.JsonObject;
  readonly seed: SDK.Seed;
  readonly capabilities: readonly Capability[];
  readonly services: SDK.ContractServices;
  evaluate(recipe: SDK.RecipeDocument, resources?: readonly object[]): GeometryResult;
  query(asset: GeometryResult, request: object | string): GeometryResult;
  process(asset: GeometryResult, request: object | string): GeometryResult;
  release(result: GeometryResult): void;
}
export interface Measurements {
  elapsedMs: number;
  stages: number;
  /** WASM heap capacity after evaluation, not allocator resident/peak usage. */
  wasmHeapBytes: number;
  /** Owned wire + typed buffer bytes transferred, excluding parsed JSON metadata. */
  transferredBytes: number;
}
/** Contains no native handle. All typed arrays and metadata are JavaScript-owned. */
export interface RuntimeResult extends GeometryResult { measurements: Measurements }
export interface WorkerPort {
  onmessage: ((event: MessageEvent) => void) | null;
  onerror: ((event: ErrorEvent) => void) | null;
  onmessageerror: ((event: MessageEvent) => void) | null;
  postMessage(message: unknown): void;
  terminate(): void;
}
export interface RuntimeOptions {
  geometryModuleURL: string | URL;
  workerURL?: string | URL;
  geometryAdapterURL?: string | URL;
  sdkURL?: string | URL;
  contractsURL?: string | URL;
  concurrency?: number;
  maxQueued?: number;
  maxInputBytes?: number;
  /** Retire module realms periodically; revoked Blob modules may stay cached. */
  maxJobsPerWorker?: number;
  timeoutMs?: number;
  /** Testing/host injection; production defaults to a module Worker. */
  workerFactory?: (url: URL) => WorkerPort;
}
export interface SubmitOptions { signal?: AbortSignal; timeoutMs?: number }
export interface RuntimeStats { active: number; queued: number; workers: number; restarts: number; rejectedLateMessages: number; disposed: boolean }
