import assert from 'node:assert/strict';
import { createServer } from 'node:http';
import { readFile, mkdir, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { chromium } from 'playwright';

const app = fileURLToPath(new URL('../', import.meta.url));
const root = path.join(app, 'dist');
const artifacts = path.join(app, 'frontend-tests/artifacts');
await mkdir(artifacts, { recursive: true });
const types = { '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript', '.css': 'text/css', '.wasm': 'application/wasm', '.json': 'application/json' };
const requests = [];
const server = createServer(async (request, response) => {
  const url = new URL(request.url, 'http://localhost');
  if (url.pathname.endsWith('/favicon.ico')) { response.writeHead(204).end(); return; }
  const relative = decodeURIComponent(url.pathname.replace(/^\/workshop\//, '')) || 'index.html';
  const target = path.resolve(root, relative);
  if (!url.pathname.startsWith('/workshop/') || !target.startsWith(`${root}${path.sep}`)) { response.writeHead(404).end(); return; }
  try {
    const bytes = await readFile(target);
    response.writeHead(200, { 'Content-Type': types[path.extname(target)] ?? 'application/octet-stream' });
    response.end(bytes); requests.push(url.pathname);
  } catch { response.writeHead(404).end(); }
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
const url = process.env.DEMO_TEST_URL ?? `http://127.0.0.1:${server.address().port}/workshop/`;
let browser;
try {
  browser = await chromium.launch({
    ...(process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE_PATH ? { executablePath: process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE_PATH } : {}),
    headless: true,
    args: ['--no-sandbox', '--use-angle=swiftshader', '--enable-unsafe-swiftshader'],
  });
  const page = await browser.newPage({ viewport: { width: 1440, height: 1000 } });
  const failures = [];
  page.on('pageerror', error => failures.push(error.message));
  page.on('console', message => { if (message.type() === 'error') failures.push(message.text()); });
  page.on('response', response => { if (response.status() >= 400) failures.push(`${response.status()} ${response.url()}`); });
  const ready = async (recipe, step) => page.waitForFunction(({ recipe, step }) => {
    const app = document.getElementById('app');
    return app.dataset.state === 'ready' && app.dataset.recipe === recipe && (step === undefined || Number(app.dataset.step) === step);
  }, { recipe, step }, { timeout: 30000 });
  await page.goto(url);
  await ready('arch');
  assert.equal(await page.locator('#gallery button').count(), 13);
  const initial = await page.locator('#triangles').innerText();
  assert.ok(Number(initial.replaceAll(',', '')) > 0);
  const beforeEdit = await page.locator('#canvas').screenshot();
  const parameter = page.locator('#parameters .lil-number input').first();
  await parameter.fill('1.5');
  await page.waitForFunction(() => document.getElementById('app').dataset.state === 'busy');
  await parameter.press('Tab'); await ready('arch');
  assert.notDeepEqual(await page.locator('#canvas').screenshot(), beforeEdit, 'editing a dimension should change the rendered geometry');
  await page.locator('#wireframe').check(); await page.locator('#normals').check();
  await page.screenshot({ path: path.join(artifacts, 'arch-wireframe-normals.png') });
  await page.locator('#wireframe').uncheck(); await page.locator('#normals').uncheck();
  await page.locator('#timeline').evaluate(input => { input.value = '0'; input.dispatchEvent(new Event('input', { bubbles: true })); });
  await ready('arch', 0); assert.ok(!(await page.locator('#step-name').innerText()).includes('Final'));
  await page.locator('#final').click(); await ready('arch');

  // Every default recipe passes through the real browser worker/WASM and GPU upload.
  const catalog = [];
  for (const button of await page.locator('#gallery button').all()) {
    const id = await button.getAttribute('data-recipe');
    await button.click(); await ready(id);
    const triangles = Number((await page.locator('#triangles').innerText()).replaceAll(',', ''));
    assert.ok(triangles > 0, `${id} should draw triangles`);
    catalog.push({ id, triangles, instances: await page.locator('#instances').innerText(), elapsed: await page.locator('#eval-time').innerText() });
  }
  await page.locator('#gallery [data-recipe=gear]').click(); await ready('gear');
  await page.locator('#timeline').evaluate(input => { input.value = '0'; input.dispatchEvent(new Event('input', { bubbles: true })); });
  await ready('gear', 0); assert.equal(await page.locator('#triangles').innerText(), '—');
  await page.locator('#next').click(); await ready('gear', 1);
  assert.ok(Number((await page.locator('#triangles').innerText()).replaceAll(',', '')) > 0);
  await page.locator('#gallery [data-recipe=chair]').click(); await ready('chair');
  await page.locator('#frame').click(); await page.screenshot({ path: path.join(artifacts, 'chair.png') });

  // Rapid switches cancel older work, invalid inputs fail visibly, reset recovers.
  await page.locator('#gallery [data-recipe=arch]').click();
  await page.locator('#gallery [data-recipe=crystals]').click();
  await page.locator('#gallery [data-recipe=rock]').click(); await ready('rock');
  await page.locator('#seed').fill('bad');
  await page.waitForFunction(() => document.getElementById('app').dataset.state === 'error');
  assert.ok((await page.locator('#status').innerText()).includes('16 hexadecimal'));
  await page.locator('#reset').click(); await ready('rock');
  await page.setViewportSize({ width: 390, height: 844 });
  await page.screenshot({ path: path.join(artifacts, 'mobile.png'), fullPage: true });
  assert.ok(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth + 1), 'mobile page must not overflow horizontally');
  assert.deepEqual(failures, []);
  if (!process.env.DEMO_TEST_URL) assert.ok(requests.some(request => request.endsWith('.wasm')), 'real WASM was served');
  const evidence = { url, catalog, errors: failures, wasmRequests: requests.filter(request => request.endsWith('.wasm')), screenshots: ['arch-wireframe-normals.png', 'chair.png', 'mobile.png'] };
  await writeFile(path.join(artifacts, 'browser.json'), JSON.stringify(evidence, null, 2));
  console.log(JSON.stringify(evidence, null, 2));
} finally {
  await browser?.close(); await new Promise(resolve => server.close(resolve));
}
