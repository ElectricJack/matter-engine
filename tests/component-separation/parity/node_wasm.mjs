// Node driver for wasm_steps.mjs: node node_wasm.mjs <matter_geometry_wasm.mjs>
// reads one JSON step per stdin line and prints one outcome line per step,
// the same protocol as matter_parity_native. The module is loaded as in
// bindings/geometry-c/wasm/run-smoke.mjs.
import {readFileSync} from 'node:fs';
import {pathToFileURL} from 'node:url';
import {WasmSteps} from './wasm_steps.mjs';

const modulePath = process.argv[2];
if (!modulePath) { console.error('usage: node node_wasm.mjs <matter_geometry_wasm.mjs>'); process.exit(2); }
const create = (await import(pathToFileURL(modulePath).href)).default;
const module = await create({wasmBinary: readFileSync(modulePath.replace(/\.mjs$/, '.wasm'))});
const runner = new WasmSteps(module);
const input = readFileSync(0, 'utf8').split('\n').filter(line => line.length);
try {
  for (const line of input) process.stdout.write(`${await runner.run(JSON.parse(line))}\n`);
} catch (error) {
  console.error(`node_wasm: ${error.message}`);
  process.exit(2);
} finally { await runner.dispose(); }
