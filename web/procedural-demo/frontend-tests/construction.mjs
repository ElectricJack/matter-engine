import assert from 'node:assert/strict';
import { mkdir, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { chromium } from 'playwright';
import { artifacts, serveBuild, launchOptions, ready } from './server.mjs';

// Separate from the fast gate: recording deliberately holds each stage long
// enough for a person to see the evolving object and construction label.
const server = await serveBuild();
await mkdir(artifacts, { recursive: true });
let browser, context;
try {
  browser = await chromium.launch(launchOptions);
  context = await browser.newContext({ viewport: { width: 1280, height: 900 }, recordVideo: { dir: artifacts, size: { width: 1280, height: 900 } } });
  const page = await context.newPage();
  await page.goto(server.url); await ready(page, 'arch');
  const count = Number(await page.locator('#timeline').getAttribute('max')) + 1;
  assert.ok(count > 2);
  const stages = [];
  for (let step = 0; step < count; step++) {
    await page.locator('#timeline').evaluate((input, value) => { input.value = String(value); input.dispatchEvent(new Event('input', { bubbles: true })); }, step);
    await ready(page, 'arch', step);
    stages.push({ step, name: await page.locator('#step-name').innerText(), triangles: await page.locator('#triangles').innerText() });
    await page.waitForTimeout(650);
  }
  const video = page.video();
  await context.close(); context = undefined;
  await video.saveAs(path.join(artifacts, 'construction.webm'));
  await writeFile(path.join(artifacts, 'construction.json'), JSON.stringify({ recipe: 'arch', stages }, null, 2) + '\n');
  console.log(`Recorded ${count} construction stages to ${path.join(artifacts, 'construction.webm')}`);
} finally {
  await context?.close(); await browser?.close(); await server.close();
}
