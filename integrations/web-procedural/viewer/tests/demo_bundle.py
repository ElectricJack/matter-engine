#!/usr/bin/env python3
"""Load only the built static demo bundle in real headless Chromium.

The default smoke checks the first WASM-generated frame in under ten seconds.
--full-demo additionally exercises every demo fixture, controls and disposal.
Neither mode exposes the source checkout through the HTTP server.
"""
import argparse
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import threading
import time
from urllib.parse import urlsplit

from browser_tests import CHROME_ARGS, check, run_demo, sha256


def run(bundle, browser_path, *, full_demo=False, screenshots=None):
    from playwright.sync_api import sync_playwright

    started = time.monotonic()
    bundle = bundle.resolve()
    check((bundle / "index.html").is_file(), f"missing built bundle: {bundle}")
    errors, http_errors, requests = [], [], []

    class Handler(SimpleHTTPRequestHandler):
        def do_GET(self):
            # Mount beneath a prefix to catch accidental domain-root fetches.
            if not urlsplit(self.path).path.startswith("/demo/"):
                self.send_error(404, "outside the demo bundle")
                return
            super().do_GET()

        def translate_path(self, path):
            return super().translate_path(path.removeprefix("/demo"))

        def send_error(self, code, message=None, explain=None):
            http_errors.append(f"{code} {self.path}")
            super().send_error(code, message, explain)

        def log_message(self, *_args):
            pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), partial(Handler, directory=str(bundle)))
    thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.05}, daemon=True)
    thread.start()
    base = f"http://127.0.0.1:{server.server_port}/demo"
    evidence = {"errors": errors, "http_errors": http_errors, "requests": requests}
    try:
        with sync_playwright() as playwright:
            browser = playwright.chromium.launch(executable_path=str(browser_path), headless=True, args=CHROME_ARGS)
            evidence["browser"] = browser.version
            context = browser.new_context(viewport={"width": 1280, "height": 860})
            context.on("request", lambda request: requests.append(request.url))
            page = context.new_page()
            page.on("pageerror", lambda error: errors.append(str(error)))
            page.set_default_timeout(7000)
            page.goto(f"{base}/index.html?seed=1")
            page.wait_for_function("window.matterDemoFailure || (window.matterDemo?.last != null)")
            failure = page.evaluate("window.matterDemoFailure ?? null")
            check(not failure, f"demo boot failed: {failure}")
            frame = page.evaluate("""() => {
              const app = window.matterDemo;
              app.render();
              const probe = Object.assign(document.createElement('canvas'), {width: 128, height: 96});
              const ctx = probe.getContext('2d');
              ctx.drawImage(app.renderer.domElement, 0, 0, probe.width, probe.height);
              const pixels = ctx.getImageData(0, 0, probe.width, probe.height).data;
              let covered = 0;
              for (let i = 0; i < pixels.length; i += 4)
                if (Math.abs(pixels[i] - pixels[0]) + Math.abs(pixels[i + 1] - pixels[1]) + Math.abs(pixels[i + 2] - pixels[2]) > 24) covered++;
              return {status: app.last.status, error: app.lastError, fixture: app.state.fixture.id,
                triangles: app.renderer.info.render.triangles, coverage: covered / (probe.width * probe.height),
                workers: window.matterRuntime.stats.workers};
            }""")
            check(frame["status"] == "ok", f"initial WASM generation failed: {frame}")
            check(frame["workers"] > 0 and frame["triangles"] > 0 and frame["coverage"] > 0.01,
                  f"generated frame is empty: {frame}")
            evidence["frame"] = frame
            if full_demo:
                run_demo(page, base, screenshots, evidence)
                if screenshots:
                    # Include controls and diagnostics, in addition to the canvas fixtures.
                    page.evaluate("window.matterRuntime.dispose()")
                    page.goto(f"{base}/index.html?fixture=WEB-VIEW-ASSEMBLY&seed=1")
                    page.wait_for_function("window.matterDemo?.last?.status === 'ok'")
                    page.evaluate("window.matterDemo.render()")
                    png = page.screenshot(path=str(screenshots / "demo-ui.png"), full_page=True)
                    evidence["ui_screenshot"] = {"file": f"{screenshots.name}/demo-ui.png",
                                                 "bytes": len(png), "sha256": sha256(png)}
            page.evaluate("window.matterDemo.dispose(); window.matterRuntime.dispose()")
            context.close()
            browser.close()
        check(not errors, f"browser page errors: {errors}")
        check(not http_errors, f"bundle HTTP errors: {http_errors}")
        outside = [url for url in requests if not url.startswith((base + "/", "blob:", "data:"))]
        check(not outside, f"demo requested files outside its bundle: {outside}")
        check(any(url.endswith(".wasm") for url in requests), "no WASM binary was loaded")
        evidence["elapsed_seconds"] = round(time.monotonic() - started, 3)
        if not full_demo:
            check(evidence["elapsed_seconds"] < 10, f"smoke exceeded ten seconds: {evidence['elapsed_seconds']}")
        evidence["result"] = "pass"
        return evidence
    except Exception as error:
        evidence.update(result="fail", error=str(error))
        error.evidence = evidence
        raise
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle-dir", required=True, type=Path)
    parser.add_argument("--browser", type=Path, default=Path("/opt/google/chrome/chrome"))
    parser.add_argument("--full-demo", action="store_true")
    parser.add_argument("--screenshots", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    evidence = {"result": "fail"}
    try:
        evidence = run(args.bundle_dir, args.browser, full_demo=args.full_demo, screenshots=args.screenshots)
        print(f"PASS: standalone demo {'end-to-end' if args.full_demo else 'smoke'} ({evidence['elapsed_seconds']:.3f} s)")
    except Exception as error:
        evidence = getattr(error, "evidence", {"result": "fail", "error": str(error)})
        raise
    finally:
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(evidence, indent=2) + "\n")


if __name__ == "__main__":
    main()
