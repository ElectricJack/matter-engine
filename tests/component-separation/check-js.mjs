import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { canonicalJson, contentDigest, Rng, hex64 } from '../../libs/MatterContracts/examples/contracts.mjs';
import { rng } from '../../MatterEngine3/shared-lib/rng.js';
const read = path => readFileSync(new URL(path, import.meta.url), 'utf8');
const load = path => JSON.parse(read(path));
for (const v of load('fixtures/rng-xoshiro128.json').vectors) {
  const words = rng(v.seed), units = rng(v.seed);
  assert.deepEqual(v.words.map(() => words.next()), v.words);
  assert.deepEqual(v.units.map(() => units.random()), v.units);
}
for (const v of load('fixtures/rng-splitmix64.json').vectors) {
  const words = new Rng(BigInt('0x' + v.seed)), units = new Rng(BigInt('0x' + v.seed));
  assert.deepEqual(v.words.map(() => hex64(words.nextU64())), v.words);
  assert.deepEqual(v.units.map(() => units.nextUnit()), v.units);
}
for (const c of load('fixtures/params.json').cases)
  assert.equal(canonicalJson(c.params), c.portable_canonical);
for (const sample of load('fixtures/documents.json')) {
  const wire = read(sample.expected_wire), document = JSON.parse(wire);
  assert.equal(canonicalJson(document), wire);
  const hash = await contentDigest(document, async bytes => new Uint8Array(createHash('sha256').update(bytes).digest()));
  assert.equal(hash.hex, sample.content_sha256);
}
console.log('PASS JS: frozen legacy xoshiro128** and portable SplitMix64 vectors, parameters, canonical documents/digests');
