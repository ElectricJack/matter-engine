#!/usr/bin/env python3
"""Real Chromium worker fixtures. No package install or fabricated fallback."""
import argparse
from functools import partial
import gzip
import hashlib
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import platform
import sys
import threading


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wasm-module", type=Path, required=True)
    parser.add_argument("--runtime-dir", type=Path)
    parser.add_argument("--browser", type=Path, default=Path("/opt/google/chrome/chrome"))
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    from playwright.sync_api import sync_playwright
    root = Path(__file__).resolve().parents[4]
    runtime = Path(__file__).resolve().parents[1]
    dist = (args.runtime_dir or runtime / "dist").resolve()
    module = args.wasm_module.resolve()
    if not args.browser.is_file() or not module.is_file() or not module.with_suffix(".wasm").is_file() or not (dist / "index.mjs").is_file():
        parser.error("Browser, compiled runtime, and real WASM module/artifact are required")
    served = set()

    class Handler(SimpleHTTPRequestHandler):
        def translate_path(self, path):
            clean = path.split("?", 1)[0]
            if clean in ("/wasm/matter_geometry_wasm.mjs", "/wasm/matter_geometry_wasm.wasm"):
                result = module if clean.endswith(".mjs") else module.with_suffix(".wasm")
            elif clean.startswith("/integrations/web-procedural/runtime/dist/"):
                result = dist / Path(clean).name
            else:
                result = Path(super().translate_path(path))
            if result.is_file():
                served.add(result)
            return str(result)

        def log_message(self, *_args):
            pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), partial(Handler, directory=str(root)))
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    evidence = {"fixtures": None, "errors": []}
    try:
        with sync_playwright() as playwright:
            browser = playwright.chromium.launch(executable_path=str(args.browser), headless=True,
                args=["--no-sandbox", "--enable-precise-memory-info", "--disable-background-networking"])
            evidence["environment"] = {"browser": browser.version, "executable": str(args.browser),
                "python": sys.version.split()[0], "platform": platform.platform(),
                "wasm_profile": "matter-wasm32-worker-v1", "concurrency": 2}
            page = browser.new_page()
            page.on("pageerror", lambda error: evidence["errors"].append(str(error)))
            page.goto(f"http://127.0.0.1:{server.server_port}/integrations/web-procedural/runtime/tests/browser.html")
            page.wait_for_function("window.runtimeReport || window.runtimeFailure", timeout=100000)
            failure = page.evaluate("window.runtimeFailure")
            if failure:
                evidence["failure"] = failure
                raise AssertionError(failure)
            evidence["fixtures"] = page.evaluate("window.runtimeReport")
            if evidence["errors"]:
                raise AssertionError(evidence["errors"])
            browser.close()
        files = []
        for path in sorted(served):
            if path.suffix not in (".mjs", ".wasm") or path.parent == runtime / "tests":
                continue
            data = path.read_bytes()
            files.append({"path": str(path.relative_to(root)) if path.is_relative_to(root) else path.name,
                "bytes": len(data), "gzip_bytes": len(gzip.compress(data, mtime=0)), "sha256": hashlib.sha256(data).hexdigest()})
        evidence["download"] = {"scope": "Unique production modules and WASM actually served, excluding test JS/HTML; raw HTTP, hypothetical per-file gzip",
            "fixtures": ["WEB-PROP", "WEB-ASSEMBLY", "WEB-STAGED"],
            "bytes": sum(f["bytes"] for f in files), "gzip_bytes": sum(f["gzip_bytes"] for f in files), "files": files}
        if any("quickjs" in f["path"].lower() for f in files):
            raise AssertionError("QuickJS artifact downloaded by browser fixtures")
        print("PASS:", ", ".join(evidence["fixtures"]["passed"]))
        print("Evidence:", args.output)
        return 0
    finally:
        server.shutdown()
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(evidence, indent=2) + "\n")


if __name__ == "__main__":
    raise SystemExit(main())
