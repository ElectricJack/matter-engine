import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { readFileSync } from 'node:fs';
import { canonicalJson, exactNumber, contentDigest, Rng, hex64, float32 } from '../examples/contracts.mjs';
const result = spawnSync(process.argv[2], { encoding: 'utf8', maxBuffer: 16 * 1024 * 1024 });
assert.equal(result.status, 0, result.stderr);
const vectors = JSON.parse(result.stdout);
for (const c of vectors.numbers) {
  const view = new DataView(new ArrayBuffer(8));view.setBigUint64(0, BigInt('0x' + c.bits), false);
  const n = view.getFloat64(0, false);
  assert.equal(exactNumber(n), c.canonical, 'binary64 ' + c.bits);
  assert.equal(createHash('sha256').update(c.canonical).digest('hex'), c.sha256);
  assert.equal(JSON.parse(c.canonical), Object.is(n, -0) ? 0 : n);
}
const pinned = JSON.parse(readFileSync(new URL('./rng-vectors.json', import.meta.url), 'utf8'));
assert.deepEqual(vectors.rng, pinned.vectors, 'frozen native SplitMix64 known answers');
for (const vector of vectors.rng) {
  const rng = new Rng(BigInt('0x' + vector.seed));
  assert.deepEqual(vector.words, vector.words.map(() => hex64(rng.nextU64())));
  const units = new Rng(BigInt('0x' + vector.seed));
  assert.deepEqual(vector.units, vector.units.map(() => units.nextUnit()));
}
const digestBytes = async bytes => new Uint8Array(createHash('sha256').update(bytes).digest());
for (const kind of ['recipe', 'asset']) {
  const document = JSON.parse(vectors[kind + '_wire']);
  assert.equal(canonicalJson(document), vectors[kind + '_wire']);
  assert.equal((await contentDigest(document, digestBytes)).hex, vectors[kind + '_digest']);
}
assert.equal(canonicalJson({ z: -0, a: 0.1 }), '{"a":0.1000000000000000055511151231257827021181583404541015625,"z":0}');
assert.equal(canonicalJson({ '\u{10000}': 1, '\ue000': 2 }), '{"\ue000":2,"\u{10000}":1}');
assert.equal(canonicalJson('\n\t\\"'), '"\\u000a\\u0009\\\\\\""');
assert.throws(() => canonicalJson(NaN));assert.throws(() => canonicalJson('\ud800'));
assert.throws(() => canonicalJson(new Date()));assert.throws(() => canonicalJson([,]));
const cyclic = {};cyclic.self = cyclic;assert.throws(() => canonicalJson(cyclic));
assert.throws(() => canonicalJson([[[0]]], { maxDepth: 1 }));
assert.throws(() => canonicalJson([1, 2], { maxBytes: 2 }));
assert.throws(() => new Rng(-1n));assert.throws(() => float32(Infinity));
assert.equal(float32(0.1), Math.fround(0.1));
console.log(`ALL PASS: C++/JS ${vectors.numbers.length} canonical numeric/SHA vectors, 4 RNG seeds, recipe and shared-assembly digests`);
