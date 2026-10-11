// Node's worker is a test host for a single ordinary worker with private memory.
// This does not claim acceptance in a browser or a production geometry binding.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { pathToFileURL } from 'node:url';
import { Worker, isMainThread, workerData, parentPort } from 'node:worker_threads';
import { createHash } from 'node:crypto';
import { canonicalJson, exactNumber, contentDigest, Rng, hex64 } from '../../../libs/MatterContracts/examples/contracts.mjs';

if (isMainThread) {
  const url = pathToFileURL(process.argv[2]).href;
  await new Promise((resolve, reject) => {
    let receivedResult = false;
    const worker = new Worker(new URL(import.meta.url), { workerData: { url } });
    worker.on('error', reject);
    worker.on('message', message => {
      if (message.error) reject(new Error(message.error));
      else { receivedResult = true; console.log(message); }
    });
    worker.on('exit', code => code === 0 && receivedResult ? resolve()
      : reject(new Error(`worker exited ${code}; fixture result received: ${receivedResult}`)));
  });
} else {
  try {
    const create = (await import(workerData.url)).default;
    // Supplying bytes avoids network/file APIs inside the compiled contract code.
    const wasmBinary = readFileSync(new URL(workerData.url.replace(/\.mjs$/, '.wasm')));
    const lines = [];
    let exitCode;
    const module = await create({ wasmBinary, noInitialRun: true,
      print: line => lines.push(line), printErr: line => process.stderr.write(`${line}\n`),
      onExit: code => { exitCode = code; } });
    assert(module.HEAPU8.buffer instanceof ArrayBuffer, 'memory must be private');
    try { module.callMain([]); }
    catch (error) { if (error.name !== 'ExitStatus' || error.status !== 0) throw error; }
    assert.equal(exitCode, 0, 'fixture exited successfully');
    if (workerData.url.endsWith('wasm_contract_vectors.mjs')) {
      const vectors = JSON.parse(lines.join('\n'));
      const pinned = JSON.parse(readFileSync(new URL('../../../libs/MatterContracts/tests/rng-vectors.json', import.meta.url)));
      assert.deepEqual(vectors.rng, pinned.vectors, 'frozen SplitMix64 vectors');
      for (const c of vectors.numbers) {
        const bits = new DataView(new ArrayBuffer(8));
        bits.setBigUint64(0, BigInt(`0x${c.bits}`), false);
        assert.equal(exactNumber(bits.getFloat64(0, false)), c.canonical);
        assert.equal(createHash('sha256').update(c.canonical).digest('hex'), c.sha256);
      }
      for (const vector of vectors.rng) {
        const rng = new Rng(BigInt(`0x${vector.seed}`));
        assert.deepEqual(vector.words, vector.words.map(() => hex64(rng.nextU64())));
        const units = new Rng(BigInt(`0x${vector.seed}`));
        assert.deepEqual(vector.units, vector.units.map(() => units.nextUnit()));
      }
      const digestBytes = async bytes => new Uint8Array(createHash('sha256').update(bytes).digest());
      for (const kind of ['recipe', 'asset']) {
        const document = JSON.parse(vectors[`${kind}_wire`]);
        assert.equal(canonicalJson(document), vectors[`${kind}_wire`]);
        assert.equal((await contentDigest(document, digestBytes)).hex, vectors[`${kind}_digest`]);
      }
      parentPort.postMessage(`PASS: WASM/JS ${vectors.numbers.length} numeric/SHA vectors, frozen RNG and recipe/asset digests; private worker memory`);
    } else {
      assert(lines.some(line => /ALL PASS|checks passed/.test(line)), 'fixture success marker');
      parentPort.postMessage(`${lines.join('\n')}; private worker memory`);
    }
  } catch (error) {
    parentPort.postMessage({ error: error.stack });
    process.exitCode = 1;
  }
}
