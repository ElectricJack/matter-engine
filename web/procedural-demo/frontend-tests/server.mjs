import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

export const app = fileURLToPath(new URL('../', import.meta.url));
export const artifacts = path.join(app, 'frontend-tests/artifacts');

/** Exercise the static build below a prefix, including worker-relative URLs. */
export async function serveBuild() {
  const root = path.join(app, 'dist');
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
  return {
    url: process.env.DEMO_TEST_URL ?? `http://127.0.0.1:${server.address().port}/workshop/`,
    requests,
    close: () => new Promise(resolve => server.close(resolve)),
  };
}

export const launchOptions = {
  ...(process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE_PATH ? { executablePath: process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE_PATH } : {}),
  headless: true,
  args: ['--no-sandbox', '--use-angle=swiftshader', '--enable-unsafe-swiftshader'],
};

export async function ready(page, recipe, step, timeout = 10000) {
  await page.waitForFunction(({ recipe, step }) => {
    const app = document.getElementById('app');
    return app.dataset.state === 'error' || (app.dataset.state === 'ready' && app.dataset.recipe === recipe && (step === undefined || Number(app.dataset.step) === step));
  }, { recipe, step }, { timeout });
  if (await page.locator('#app').getAttribute('data-state') === 'error') {
    throw new Error(`${recipe}: ${await page.locator('#status').innerText()}`);
  }
}
