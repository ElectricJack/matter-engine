import assert from 'node:assert/strict';
import { mkdir, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { chromium } from 'playwright';
import { artifacts, serveBuild, launchOptions, ready as waitReady } from './server.mjs';

const server = await serveBuild();
const {url, requests} = server;
await mkdir(artifacts, { recursive: true });
let browser;
try {
  browser = await chromium.launch(launchOptions);
  const page = await browser.newPage({ viewport: { width: 1440, height: 1000 } });
  const failures = [];
  page.on('pageerror', error => failures.push(error.message));
  page.on('console', message => { if (message.type() === 'error') failures.push(message.text()); });
  page.on('response', response => { if (response.status() >= 400) failures.push(`${response.status()} ${response.url()}`); });
  const ready = (recipe, step) => waitReady(page, recipe, step);
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
  const catalog = [], construction = [];
  for (const button of await page.locator('#gallery button').all()) {
    const id = await button.getAttribute('data-recipe');
    await button.click(); await ready(id);
    const triangles = Number((await page.locator('#triangles').innerText()).replaceAll(',', ''));
    assert.ok(triangles > 0, `${id} should draw triangles`);
    catalog.push({ id, triangles, instances: await page.locator('#instances').innerText(), elapsed: await page.locator('#eval-time').innerText() });
    const count = Number(await page.locator('#timeline').getAttribute('max')) + 1;
    const steps = [];
    for (let step = 0; step < count; step++) {
      await page.locator('#timeline').evaluate((input, value) => { input.value = String(value); input.dispatchEvent(new Event('input', { bubbles: true })); }, step);
      await ready(id, step);
      const label = await page.locator('#step-name').innerText();
      const triangles = await page.locator('#triangles').innerText();
      assert.ok(label.length > 0);
      if (triangles !== '—') assert.ok(Number(triangles.replaceAll(',', '')) > 0, `${id} stage ${step} must render geometry`);
      else assert.ok((await page.locator('#status').innerText()).includes('Profile step'), `${id} stage ${step} must explain the absence of solid geometry`);
      steps.push({ step, label, triangles });
    }
    assert.equal(steps.at(-1).label, 'Final object');
    construction.push({ id, steps });
  }

  // Maximum chain count crosses the heavy-object threshold. Test actual native
  // simplification and camera-distance selection, then return to full detail.
  await page.locator('#gallery [data-recipe=chain]').click(); await ready('chain');
  const links = page.locator('#parameters .lil-number input').first();
  await links.fill('9'); await links.press('Tab');
  await page.waitForFunction(() => document.getElementById('triangles').textContent.replaceAll(',', '') === '1944');
  await ready('chain');
  // Parameter edits preserve the current camera. Fit the larger nine-link
  // object before testing a relative zoom distance.
  await page.locator('#frame').click();
  const baseTriangles = Number(await page.locator('#canvas').getAttribute('data-rendered-triangles'));
  await page.locator('#canvas').hover(); await page.mouse.wheel(0, 2400);
  await page.waitForFunction(() => document.getElementById('canvas').dataset.detail === 'reduced');
  const reducedTriangles = Number(await page.locator('#canvas').getAttribute('data-rendered-triangles'));
  assert.ok(reducedTriangles > 0 && reducedTriangles < baseTriangles);
  await page.screenshot({ path: path.join(artifacts, 'chain-lod.png') });
  await page.locator('#frame').click();
  await page.waitForFunction(() => document.getElementById('canvas').dataset.detail === 'full');
  const lod = { baseTriangles, reducedTriangles };
  await page.locator('#gallery [data-recipe=gear]').click(); await ready('gear');
  await page.locator('#timeline').evaluate(input => { input.value = '0'; input.dispatchEvent(new Event('input', { bubbles: true })); });
  await ready('gear', 0); assert.equal(await page.locator('#triangles').innerText(), '—');
  await page.locator('#next').click(); await ready('gear', 1);
  assert.ok(Number((await page.locator('#triangles').innerText()).replaceAll(',', '')) > 0);
  await page.locator('#gallery [data-recipe=chair]').click(); await ready('chair');
  await page.locator('#frame').click(); await page.screenshot({ path: path.join(artifacts, 'chair-workshop.png') });

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
  const evidence = { url, catalog, construction, lod, errors: failures, wasmRequests: requests.filter(request => request.endsWith('.wasm')), screenshots: ['arch-wireframe-normals.png', 'chair-workshop.png', 'chain-lod.png', 'mobile.png'] };
  await writeFile(path.join(artifacts, 'browser.json'), JSON.stringify(evidence, null, 2));
  console.log(JSON.stringify(evidence, null, 2));
} finally {
  await browser?.close(); await server.close();
}
