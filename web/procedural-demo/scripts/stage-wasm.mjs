import { cp, mkdir, readFile, rm } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const app = fileURLToPath(new URL('../', import.meta.url));
const source = process.env.MATTER_WASM_DIST
  ? path.resolve(process.env.MATTER_WASM_DIST)
  : path.resolve(app, '../matter-procedural-wasm/dist');
const destination = path.join(app, 'public/matter-procedural-wasm');
try {
  // Check before replacing a previously staged package. Keep all relative imports intact.
  for (const name of ['index.mjs', 'worker.mjs', 'matter_geometry_wasm.mjs', 'matter_geometry_wasm.wasm']) {
    await readFile(path.join(source, name));
  }
  if (source === destination) throw new Error('MATTER_WASM_DIST must point outside the staging directory');
  await mkdir(path.dirname(destination), { recursive: true });
  await rm(destination, { recursive: true, force: true });
  await cp(source, destination, { recursive: true });
  console.log(`Staged Matter WASM package from ${source}`);
} catch (error) {
  console.error(`Cannot stage the Matter WASM package: ${error.message}\n` +
    'Build it first: python3 web/matter-procedural-wasm/build.py --emsdk /path/to/emsdk\n' +
    'Or set MATTER_WASM_DIST to an existing complete package dist directory.');
  process.exitCode = 1;
}
