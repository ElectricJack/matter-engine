import {PROTOCOL, RuntimeError, bounded, envelope, jsonCopy, same} from './protocol.mjs';
import type {Envelope, Reply, Start} from './protocol.mjs';
import type {RuntimeJob, RuntimeOptions, RuntimeResult, RuntimeStats, SubmitOptions, WorkerPort} from './types.mjs';
export {RuntimeError} from './protocol.mjs';
export type * from './types.mjs';

interface Pending {
  input: RuntimeJob; envelope: Envelope; serial: number; timeoutMs: number;
  resolve: (result: RuntimeResult) => void; reject: (error: RuntimeError) => void;
  removeAbort: () => void; timer?: ReturnType<typeof setTimeout>;
}
interface Slot { worker?: WorkerPort; pending?: Pending; completed: number }

export class ProceduralRuntime {
  #slots: Slot[]; #queue: Pending[] = []; #pending = new Map<string, Pending>();
  #options: Required<Omit<RuntimeOptions, 'workerFactory'>>;
  #config: Start['config']; #factory: (url: URL) => WorkerPort;
  #serial = 0; #disposed = false; #restarts = 0; #late = 0;
  #current?: {source_revision: string; generation_epoch: string};
  constructor(options: RuntimeOptions) {
    const url = (value: string | URL) => new URL(value, import.meta.url);
    this.#options = {
      geometryModuleURL: url(options.geometryModuleURL),
      workerURL: url(options.workerURL ?? './worker.mjs'),
      geometryAdapterURL: url(options.geometryAdapterURL ?? '../../../../bindings/geometry-c/wasm/geometry.mjs'),
      sdkURL: url(options.sdkURL ?? '../../../../packages/matter-authoring/index.mjs'),
      contractsURL: url(options.contractsURL ?? '../../../../libs/MatterContracts/examples/contracts.mjs'),
      concurrency: bounded(options.concurrency ?? 2, 1, 8, 'concurrency'),
      maxQueued: bounded(options.maxQueued ?? 32, 0, 1024, 'maxQueued'),
      maxInputBytes: bounded(options.maxInputBytes ?? 8 * 1024 * 1024, 1, 64 * 1024 * 1024, 'maxInputBytes'),
      maxJobsPerWorker: bounded(options.maxJobsPerWorker ?? 32, 1, 128, 'maxJobsPerWorker'),
      timeoutMs: bounded(options.timeoutMs ?? 10000, 1, 60000, 'timeoutMs'),
    };
    this.#config = {
      geometryModuleURL: String(this.#options.geometryModuleURL), geometryAdapterURL: String(this.#options.geometryAdapterURL),
      sdkURL: String(this.#options.sdkURL), contractsURL: String(this.#options.contractsURL),
    };
    this.#factory = options.workerFactory ?? (url => new Worker(url, {type: 'module'}));
    this.#slots = Array.from({length: this.#options.concurrency}, () => ({completed: 0}));
  }
  get stats(): RuntimeStats {
    return {active: this.#slots.filter(s => s.pending).length, queued: this.#queue.length,
      workers: this.#slots.filter(s => s.worker).length, restarts: this.#restarts,
      rejectedLateMessages: this.#late, disposed: this.#disposed};
  }
  #matchesCurrent(job: Envelope): boolean {
    return !this.#current || (job.source_revision === this.#current.source_revision && job.generation_epoch === this.#current.generation_epoch);
  }
  /** Set admission identity and invalidate every queued/running older revision. */
  setCurrent(sourceRevision: string, generationEpoch: bigint | number): void {
    if (this.#disposed) throw new RuntimeError('disposed', 'Runtime is disposed');
    this.#current = envelope({request_id: 'admission', source_revision: sourceRevision, generation_epoch: generationEpoch});
    for (const p of [...this.#pending.values()]) if (!this.#matchesCurrent(p.envelope))
      this.#cancel(p, new RuntimeError('stale_result', 'Source revision or generation epoch was invalidated'), false);
    this.#pump();
  }
  submit(input: RuntimeJob, options: SubmitOptions = {}): Promise<RuntimeResult> {
    try {
      if (this.#disposed) throw new RuntimeError('disposed', 'Runtime is disposed');
      if (options.signal?.aborted) throw new RuntimeError('cancelled', 'Job was already cancelled');
      const identity = envelope(input.job);
      if (!this.#matchesCurrent(identity)) throw new RuntimeError('stale_result', 'Job does not match the current source revision and epoch');
      if (this.#pending.has(identity.request_id)) throw new RuntimeError('invalid_input', 'Request ID is already active');
      if (this.#slots.every(s => s.pending) && this.#queue.length >= this.#options.maxQueued)
        throw new RuntimeError('resource_limit', 'Worker queue is full');
      const timeoutMs = bounded(options.timeoutMs ?? this.#options.timeoutMs, 1, 60000, 'timeoutMs');
      const maxStages = bounded(input.maxStages ?? 32, 1, 512, 'maxStages');
      // Clone contract data now, before queuing. No borrowed caller objects or
      // detached caller buffers cross the asynchronous boundary.
      const common = {job: {...identity, generation_epoch: BigInt(input.job.generation_epoch)},
        maxStages, ...(input.budget === undefined ? {} : {budget: structuredClone(input.budget)})};
      let copy: RuntimeJob;
      if (input.kind === 'evaluate') copy = {...common, kind: 'evaluate', request: jsonCopy(input.request)};
      else if (input.kind === 'author') {
        if (!input.snapshot || typeof input.snapshot.source !== 'string' || typeof input.snapshot.uri !== 'string' || !input.snapshot.uri)
          throw new RuntimeError('invalid_input', 'An immutable module source and source URI are required');
        copy = {...common, kind: 'author', snapshot: {source: input.snapshot.source, uri: input.snapshot.uri}, parameters: jsonCopy(input.parameters ?? {}), seed: input.seed ?? 1n};
      } else throw new RuntimeError('invalid_input', 'Unknown job kind');
      const bytes = new TextEncoder().encode(JSON.stringify(copy, (_k, v: unknown) => typeof v === 'bigint' ? v.toString() : v)).length;
      if (bytes > this.#options.maxInputBytes) throw new RuntimeError('resource_limit', 'Job input exceeds maxInputBytes');
      if (this.#serial === Number.MAX_SAFE_INTEGER) throw new RuntimeError('resource_limit', 'Dispatch identity exhausted');
      const serial = ++this.#serial;
      return new Promise((resolve, reject) => {
        const p: Pending = {input: copy, envelope: identity, serial, timeoutMs, resolve, reject, removeAbort: () => {}};
        const abort = () => this.#cancel(p, new RuntimeError('cancelled', 'Job was cancelled'));
        if (options.signal) {
          options.signal.addEventListener('abort', abort, {once: true});
          p.removeAbort = () => options.signal!.removeEventListener('abort', abort);
        }
        this.#pending.set(identity.request_id, p); this.#queue.push(p); this.#pump();
      });
    } catch (error) {
      return Promise.reject(error instanceof RuntimeError ? error : new RuntimeError('invalid_input', String(error)));
    }
  }
  cancel(requestId: string): boolean {
    const p = this.#pending.get(requestId);
    if (!p) return false;
    this.#cancel(p, new RuntimeError('cancelled', 'Job was cancelled')); return true;
  }
  #settle(p: Pending, result?: RuntimeResult, error?: RuntimeError): void {
    clearTimeout(p.timer); p.removeAbort(); this.#pending.delete(p.envelope.request_id);
    if (error) p.reject(error); else p.resolve(result!);
  }
  #kill(slot: Slot): void {
    if (slot.worker) {
      const worker = slot.worker;
      slot.worker = undefined;
      worker.onmessage = null; worker.onerror = null; worker.onmessageerror = null;
      worker.terminate(); ++this.#restarts;
      slot.completed = 0;
    }
  }
  #cancel(p: Pending, error: RuntimeError, pump = true): void {
    if (this.#pending.get(p.envelope.request_id) !== p) return;
    const slot = this.#slots.find(s => s.pending === p);
    if (slot) { this.#kill(slot); slot.pending = undefined; }
    else this.#queue = this.#queue.filter(item => item !== p);
    this.#settle(p, undefined, error); if (pump) this.#pump();
  }
  /** Restart a worker; its current job fails and no handle survives. */
  restart(index: number): void {
    if (this.#disposed) throw new RuntimeError('disposed', 'Runtime is disposed');
    const slot = this.#slots[bounded(index, 0, this.#slots.length - 1, 'worker index')];
    if (slot.pending) this.#cancel(slot.pending, new RuntimeError('cancelled', 'Worker was restarted'), false);
    else this.#kill(slot);
    this.#pump();
  }
  #worker(slot: Slot): WorkerPort {
    if (slot.worker) return slot.worker;
    const worker = this.#factory(new URL(this.#options.workerURL));
    slot.worker = worker;
    worker.onmessage = event => {
      const reply = event.data as Reply, p = slot.pending;
      // The worker object and serial reject completions from terminated workers,
      // including reuse of the same public request ID in a new generation.
      if (slot.worker !== worker || !p || reply?.protocol !== PROTOCOL || reply.serial !== p.serial) { ++this.#late; return; }
      if (!same(p.envelope, reply.envelope) || !this.#matchesCurrent(p.envelope) ||
          (reply.type === 'result' && !same(p.envelope, reply.result?.value?.job))) {
        ++this.#late; this.#cancel(p, new RuntimeError('stale_result', 'Worker returned a mismatched job envelope')); return;
      }
      if (reply.type !== 'result' && reply.type !== 'error') {
        this.#cancel(p, new RuntimeError('backend_failure', 'Invalid worker response')); return;
      }
      slot.pending = undefined;
      this.#settle(p, reply.type === 'result' ? reply.result : undefined,
        reply.type === 'error' ? new RuntimeError(reply.code, reply.message, reply.diagnostics, reply.status) : undefined);
      if (++slot.completed >= this.#options.maxJobsPerWorker || (reply.type === 'error' && reply.code === 'backend_failure')) this.#kill(slot);
      this.#pump();
    };
    const failure = () => {
      if (slot.worker !== worker) return;
      if (slot.pending) this.#cancel(slot.pending, new RuntimeError('backend_failure', 'Worker failed or could not deserialize a message'));
      else this.#kill(slot);
    };
    worker.onerror = event => { event.preventDefault(); failure(); };
    worker.onmessageerror = failure;
    return worker;
  }
  #pump(): void {
    if (this.#disposed) return;
    for (const slot of this.#slots) {
      if (slot.pending || !this.#queue.length) continue;
      const p = this.#queue.shift()!; slot.pending = p;
      p.timer = setTimeout(() => this.#cancel(p, new RuntimeError('resource_limit', 'Worker execution deadline exceeded')), p.timeoutMs);
      try {
        const message: Start = {protocol: PROTOCOL, type: 'run', serial: p.serial, envelope: p.envelope, input: p.input, config: this.#config};
        this.#worker(slot).postMessage(message);
      } catch (error) { this.#cancel(p, new RuntimeError('backend_failure', `Worker dispatch failed: ${String(error)}`), false); }
    }
    // A failed construction can leave queued work after the last slot.
    if (this.#queue.length && this.#slots.some(s => !s.pending)) queueMicrotask(() => this.#pump());
  }
  dispose(): void {
    if (this.#disposed) return;
    this.#disposed = true;
    for (const p of [...this.#pending.values()]) this.#cancel(p, new RuntimeError('disposed', 'Runtime was disposed'), false);
    for (const slot of this.#slots) this.#kill(slot);
  }
}
