// node_wasm.mjs, but answers each stdin line as it arrives so the
// caller can time individual steps (same WasmSteps protocol).
import {createInterface} from 'node:readline';
import {pathToFileURL} from 'node:url';
import {WasmSteps} from '../../../../tests/component-separation/parity/wasm_steps.mjs';
const create = (await import(pathToFileURL(process.argv[2]).href)).default;
const runner = new WasmSteps(await create({}));
process.stdout.write('{"ready":1}\n');
for await (const line of createInterface({input: process.stdin})) {
  if (line.length) process.stdout.write(`${await runner.run(JSON.parse(line))}\n`);
}
await runner.dispose();
