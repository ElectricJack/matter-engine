import * as contracts from './contracts.mjs';
import { contractServices } from './rng.mjs';
import { copyJson, freeze } from './values.mjs';

export function prepare(input, digestBytes) {
  const { maxBytes, ...limits } = input.limits;
  return Object.freeze({ ...freeze({ ...input, limits }), seed: BigInt(`0x${input.seed}`),
    services: contractServices(contracts, digestBytes) });
}
export function serialize(value, limits) {
  // Reject values JSON.stringify would erase/coerce before the native decoder.
  return contracts.canonicalJson(copyJson(value, limits), {
    maxDepth: limits.maxDepth, maxNodes: limits.maxValues,
    maxBytes: limits.maxBytes, maxStringBytes: limits.maxStringBytes });
}
