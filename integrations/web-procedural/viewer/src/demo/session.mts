// Regeneration session: one live AssetView, replaced atomically. Newer requests
// supersede older ones through the runtime's epoch admission and an
// AbortSignal; a failed generation keeps the last good view on screen.
import type {Object3D} from 'three';
import {createAssetView, type AssetSource, type AssetView, type AssetViewOptions, type MeshBuffers} from '../adapter.mjs';
import {ViewerError, type ViewerDiagnostic} from '../errors.mjs';
import type {DemoFixture, JsonObject} from './fixtures.mjs';

/** The slice of `ProceduralRuntime` the demo uses, typed structurally so the
 *  viewer has no compile-time dependency on the runtime package (and the
 *  runtime none on the viewer). tests/type_checks.mts asserts compatibility. */
export interface DemoRuntimeJob {
  readonly kind: 'author';
  readonly job: {readonly request_id: string; readonly source_revision: string; readonly generation_epoch: bigint};
  readonly snapshot: {readonly source: string; readonly uri: string};
  readonly parameters: JsonObject;
  readonly seed: bigint;
}
export interface DemoRuntimeResult {
  readonly value: {readonly kind: string; readonly asset?: unknown; readonly resources?: unknown};
  readonly meshes: readonly MeshBuffers[];
  readonly measurements?: {readonly elapsedMs: number; readonly transferredBytes: number; readonly wasmHeapBytes: number};
}
export interface DemoRuntime {
  setCurrent(sourceRevision: string, generationEpoch: bigint): void;
  submit(job: DemoRuntimeJob, options: {signal?: AbortSignal; timeoutMs?: number}): Promise<DemoRuntimeResult>;
  readonly stats: object;
}

export type FailureStage = 'input' | 'runtime' | 'adapter';
export interface DemoFailure {
  readonly stage: FailureStage;
  readonly code: string;
  readonly message: string;
  readonly diagnostics: readonly unknown[];
}
export interface GenerateRequest {
  readonly fixture: DemoFixture;
  readonly parameters: JsonObject;
  readonly seed: bigint;
  readonly viewOptions?: AssetViewOptions;
}
export type GenerateOutcome =
  | {readonly status: 'ok'; readonly epoch: number; readonly view: AssetView; readonly elapsedMs: number; readonly runtimeMs: number | null}
  | {readonly status: 'superseded'; readonly epoch: number}
  | {readonly status: 'error'; readonly epoch: number; readonly error: DemoFailure};

export interface SessionOptions {
  readonly sourceRevision?: string;
  readonly timeoutMs?: number;
  /** Base URL for asset fixtures that fetch repository files. */
  readonly baseURL: URL;
}

const SUPERSEDED = new Set(['cancelled', 'stale_result']);

function failure(stage: FailureStage, error: unknown): DemoFailure {
  if (error instanceof ViewerError) return {stage, code: error.code, message: error.message, diagnostics: error.diagnostics};
  const e = (error ?? {}) as {code?: unknown; message?: unknown; diagnostics?: unknown};
  return {stage, code: typeof e.code === 'string' ? e.code : 'backend_failure',
    message: typeof e.message === 'string' ? e.message : String(error),
    diagnostics: Array.isArray(e.diagnostics) ? e.diagnostics : []};
}

export class DemoSession {
  readonly #runtime: DemoRuntime;
  readonly #parent: Object3D;
  readonly #options: SessionOptions;
  #epoch = 0;
  #controller: AbortController | null = null;
  #view: AssetView | null = null;
  #disposed = false;

  constructor(runtime: DemoRuntime, parent: Object3D, options: SessionOptions) {
    this.#runtime = runtime; this.#parent = parent; this.#options = options;
  }
  get view(): AssetView | null { return this.#view; }
  get epoch(): number { return this.#epoch; }
  get disposed(): boolean { return this.#disposed; }

  async generate(request: GenerateRequest): Promise<GenerateOutcome> {
    if (this.#disposed) return {status: 'error', epoch: this.#epoch, error: {stage: 'input', code: 'disposed', message: 'session is disposed', diagnostics: []}};
    const epoch = ++this.#epoch;
    this.#controller?.abort();
    const controller = this.#controller = new AbortController();
    const start = performance.now();
    const current = () => epoch === this.#epoch && !this.#disposed;
    let source: AssetSource, runtimeMs: number | null = null;
    try {
      const {fixture} = request;
      if (fixture.kind === 'recipe') {
        const revision = this.#options.sourceRevision ?? 'web-demo';
        this.#runtime.setCurrent(revision, BigInt(epoch));
        const result = await this.#runtime.submit({kind: 'author',
          job: {request_id: `${fixture.id}#${epoch}`, source_revision: revision, generation_epoch: BigInt(epoch)},
          snapshot: {source: fixture.source, uri: `fixture://${fixture.id}`},
          parameters: request.parameters, seed: request.seed}, {signal: controller.signal, timeoutMs: this.#options.timeoutMs});
        runtimeMs = result.measurements?.elapsedMs ?? null;
        source = result as AssetSource;
      } else source = await fixture.load({signal: controller.signal, baseURL: this.#options.baseURL});
    } catch (error) {
      if (!current()) return {status: 'superseded', epoch};
      const code = (error as {code?: unknown} | null)?.code;
      if (typeof code === 'string' && SUPERSEDED.has(code) && controller.signal.aborted) return {status: 'superseded', epoch};
      return {status: 'error', epoch, error: failure('runtime', error)};
    }
    if (!current()) return {status: 'superseded', epoch};
    let view: AssetView;
    try { view = createAssetView(source, {...request.viewOptions, ...request.fixture.viewOptions}); }
    catch (error) { return {status: 'error', epoch, error: failure('adapter', error)}; }
    // Swap: attach the new view, then release the old one's GPU resources.
    this.#parent.add(view.root);
    const previous = this.#view;
    this.#view = view;
    previous?.dispose();
    this.#controller = null;
    return {status: 'ok', epoch, view, elapsedMs: performance.now() - start, runtimeMs};
  }

  /** Cancel in-flight work and release the current view. Idempotent. */
  dispose(): void {
    if (this.#disposed) return;
    this.#disposed = true;
    this.#controller?.abort();
    this.#view?.dispose();
    this.#view = null;
  }
}

export type {ViewerDiagnostic};
