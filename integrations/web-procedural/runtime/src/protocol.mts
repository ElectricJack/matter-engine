import type {ErrorCode, Job, RuntimeJob, RuntimeResult} from './types.mjs';
export const PROTOCOL = 1;
export class RuntimeError extends Error {
  constructor(readonly code: ErrorCode, message: string, readonly diagnostics: readonly unknown[] = [], readonly status?: number) {
    super(message); this.name = 'RuntimeError';
  }
}
export interface Envelope { request_id: string; source_revision: string; generation_epoch: string }
export function envelope(job: Job): Envelope {
  if (!job || typeof job.request_id !== 'string' || !job.request_id || typeof job.source_revision !== 'string' || !job.source_revision ||
      new TextEncoder().encode(job.request_id).length > 65536 || new TextEncoder().encode(job.source_revision).length > 65536)
    throw new RuntimeError('invalid_input', 'Nonempty bounded request ID and source revision are required');
  const epoch = job.generation_epoch;
  if ((typeof epoch !== 'bigint' && (typeof epoch !== 'number' || !Number.isSafeInteger(epoch))) || BigInt(epoch) < 0n || BigInt(epoch) > 0xffffffffffffffffn)
    throw new RuntimeError('invalid_input', 'Generation epoch must be an exact uint64');
  return {request_id: job.request_id, source_revision: job.source_revision, generation_epoch: BigInt(epoch).toString(16).padStart(16, '0')};
}
export function same(a: Envelope, b: Envelope | undefined): boolean {
  return !!b && a.request_id === b.request_id && a.source_revision === b.source_revision && a.generation_epoch === b.generation_epoch;
}
export function bounded(value: number, min: number, max: number, name: string): number {
  if (!Number.isSafeInteger(value) || value < min || value > max) throw new RuntimeError('invalid_input', `${name} must be an integer in [${min}, ${max}]`);
  return value;
}
// Recipes and parameters are JSON contract data. Reject values structuredClone
// could preserve but JSON would silently lose; job/budget uint64 is separate.
export function jsonCopy<T>(value: T): T {
  const text = JSON.stringify(value, (_key, v: unknown) => {
    if (v === undefined || typeof v === 'function' || typeof v === 'symbol' || typeof v === 'bigint' ||
        (typeof v === 'number' && !Number.isFinite(v))) throw new RuntimeError('invalid_input', 'Inputs must be finite JSON values');
    return v;
  });
  if (typeof text !== 'string') throw new RuntimeError('invalid_input', 'JSON input is required');
  return JSON.parse(text) as T;
}
export function freeze<T>(value: T): T {
  if (value && typeof value === 'object') { for (const child of Object.values(value)) freeze(child); Object.freeze(value); }
  return value;
}
export interface Start {
  protocol: 1; type: 'run'; serial: number; envelope: Envelope; input: RuntimeJob;
  config: {geometryModuleURL: string; geometryAdapterURL: string; sdkURL: string; contractsURL: string};
}
export type Reply = {protocol: 1; serial: number; envelope: Envelope} & (
  {type: 'result'; result: RuntimeResult} |
  {type: 'error'; code: ErrorCode; message: string; diagnostics: unknown[]; status?: number});
