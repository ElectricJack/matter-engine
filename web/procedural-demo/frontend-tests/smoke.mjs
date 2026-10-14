import assert from 'node:assert/strict';
import { mkdir, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { chromium } from 'playwright';
import { PNG } from 'pngjs';
import { artifacts, serveBuild, launchOptions, ready } from './server.mjs';

const start = performance.now();
const server = await serveBuild();
let browser;
// A wall-clock watchdog also bounds failed readiness/selection paths.
const deadline = setTimeout(() => { console.error('Gallery smoke exceeded 20 seconds'); process.exit(1); }, 20000);
try {
  await mkdir(artifacts, { recursive: true });
  browser = await chromium.launch(launchOptions);
  const page = await browser.newPage({ viewport: { width: 1280, height: 900 }, deviceScaleFactor: 1 });
  const failures = [], workers = [];
  page.on('worker', worker => workers.push(worker.url()));
  page.on('pageerror', error => failures.push(error.message));
  page.on('console', message => { if (message.type() === 'error') failures.push(message.text()); });
  page.on('response', response => { if (response.status() >= 400) failures.push(`${response.status()} ${response.url()}`); });
  await page.goto(server.url); await ready(page, 'arch');
  const ids = await page.locator('#gallery button').evaluateAll(buttons => buttons.map(button => button.dataset.recipe));
  assert.equal(ids.length, 13);
  const catalog = [];
  for (const id of ids) {
    await page.locator(`#gallery [data-recipe=${id}]`).click(); await ready(page, id);
    const triangles = Number((await page.locator('#triangles').innerText()).replaceAll(',', ''));
    assert.ok(triangles > 0, `${id}: native geometry must contain triangles`);
    const screenshot = await page.locator('#canvas').screenshot();
    const pixels = PNG.sync.read(screenshot);
    // Clear geometry through the real invalid-input path. Camera, lighting and
    // grid stay identical, yielding an empty-frame reference for this object.
    await page.locator('#seed').fill('bad');
    await page.waitForFunction(() => document.getElementById('app').dataset.state === 'error');
    const empty = PNG.sync.read(await page.locator('#canvas').screenshot());
    await page.locator('#seed').evaluate(input => { input.value = '0000000000000001'; });
    let objectPixels = 0;
    for (let i = 0; i < pixels.data.length; i += 4) {
      // Canvas screenshots include overlaid DOM. Exclude the status and view
      // toolbar strips so their invalid-input text cannot count as geometry.
      const row = Math.floor(i / 4 / pixels.width);
      if (row < 64 || row >= pixels.height - 64) continue;
      if (Math.max(...[0, 1, 2].map(channel => Math.abs(pixels.data[i + channel] - empty.data[i + channel]))) > 24) objectPixels++;
    }
    assert.ok(objectPixels > 500, `${id}: frame must contain visible object pixels (${objectPixels})`);
    await writeFile(path.join(artifacts, `${id}.png`), screenshot);
    catalog.push({ id, triangles, objectPixels });
  }
  assert.deepEqual(failures, []);
  assert.equal(workers.length, 1, 'sequential gallery evaluation must reuse one module worker');
  if (!process.env.DEMO_TEST_URL) assert.ok(server.requests.some(request => request.endsWith('.wasm')), 'real WASM must be served');
  await browser.close(); browser = undefined; await server.close();
  const elapsedMs = performance.now() - start;
  assert.ok(elapsedMs < 20000, `smoke took ${elapsedMs} ms`);
  const evidence = { elapsedMs, catalog, workers, errors: failures, wasmRequests: server.requests.filter(request => request.endsWith('.wasm')) };
  await writeFile(path.join(artifacts, 'smoke.json'), JSON.stringify(evidence, null, 2) + '\n');
  console.log(`Gallery smoke: 13 visible objects, one worker, ${(elapsedMs / 1000).toFixed(2)} seconds`);
} finally {
  clearTimeout(deadline); await browser?.close(); await server.close();
}
