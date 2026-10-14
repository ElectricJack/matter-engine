"""AQ resource-gated entry for the fast, renderer-free Node package tests."""
from pathlib import Path
import subprocess


def test_wasm_package():
    package = Path(__file__).resolve().parents[1]
    subprocess.run(['node', '--test', str(package / 'tests/package.test.mjs')], check=True, timeout=90)


def test_browser_worker_package():
    """Serve only dist: verifies the deployable package has no repo-relative imports."""
    from functools import partial
    from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
    import threading
    from playwright.sync_api import sync_playwright

    package = Path(__file__).resolve().parents[1]
    server = ThreadingHTTPServer(('127.0.0.1', 0), partial(SimpleHTTPRequestHandler, directory=str(package / 'dist')))
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with sync_playwright() as p:
            browser = p.chromium.launch(executable_path='/opt/google/chrome/chrome', headless=True, args=['--no-sandbox'])
            page = browser.new_page()
            errors = []
            page.on('pageerror', lambda error: errors.append(str(error)))
            page.goto(f'http://127.0.0.1:{server.server_port}')
            result = page.evaluate('''async () => {
                const {createProceduralEngine, ProceduralError, transferables} = await import('/index.mjs');
                const {RecipeBuilder} = await import('/authoring/index.mjs');
                const b = new RecipeBuilder({source: {id: 'browser', uri: 'test://browser', revision: '1'}, seed: 1n});
                b.output(b.sphere('ball', 1, 12));
                const engine = await createProceduralEngine();
                try {
                    const r = await engine.evaluate(b, {lodRatios: [0.5]});
                    const abort = new AbortController();
                    const cancelled = engine.evaluate(b, {signal: abort.signal}).catch(e => e);
                    abort.abort();
                    const error = await cancelled;
                    if (!(error instanceof ProceduralError) || error.code !== 'cancelled') throw Error('abort failed');
                    const next = await engine.evaluate(b);
                    engine.dispose();
                    const moved = structuredClone(r, {transfer: transferables(r)});
                    return {triangles: next.meshes[0].indices.length / 3,
                        lodTriangles: moved.lods[0].meshes[0].indices.length / 3,
                        detached: r.meshes[0].positions.byteLength === 0,
                        shared: typeof SharedArrayBuffer !== 'undefined' && moved.meshes[0].positions.buffer instanceof SharedArrayBuffer};
                } finally {engine.dispose();}
            }''')
            assert result['triangles'] == 264
            assert 0 < result['lodTriangles'] < 264
            assert result['detached'] and not result['shared']
            assert not errors, errors
            browser.close()
    finally:
        server.shutdown()
        server.server_close()
        thread.join()
