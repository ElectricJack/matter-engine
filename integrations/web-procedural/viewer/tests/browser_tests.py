#!/usr/bin/env python3
"""Real-Chromium viewer fixtures: adapter pixels and GPU accounting, end-to-end
regeneration through the worker runtime, the demo UI, and the runtime-only
isolation checks. No package install or fabricated fallback: a missing browser,
compiler, WASM module or compiled package is an error, not a skip."""
import argparse
from functools import partial
import hashlib
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path, PurePosixPath
import platform
import re
import shutil
import subprocess
import sys
import threading

CHROME_ARGS = ["--no-sandbox", "--enable-precise-memory-info", "--disable-background-networking",
               # Deterministic software WebGL2 so pixel fixtures do not depend on the host GPU.
               "--use-angle=swiftshader", "--enable-unsafe-swiftshader"]
RUNTIME_DIST = "/integrations/web-procedural/runtime/dist/"
VIEWER_DIST = "/integrations/web-procedural/viewer/dist/"
THREE_GLOBAL_TEXTURES = 1
IMPORT_RE = re.compile(r"""(?:\bfrom\s*|\bimport\s*\(?\s*)['"]([^'"]+)['"]""")


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def make_server(root, module, runtime_dist, viewer_dist, served, http_errors):
    class Handler(SimpleHTTPRequestHandler):
        def translate_path(self, path):
            clean = path.split("?", 1)[0].split("#", 1)[0]
            if clean in ("/wasm/matter_geometry_wasm.mjs", "/wasm/matter_geometry_wasm.wasm"):
                result = module if clean.endswith(".mjs") else module.with_suffix(".wasm")
            elif clean.startswith(RUNTIME_DIST):
                result = runtime_dist / PurePosixPath(clean).name
            elif clean.startswith(VIEWER_DIST):
                parts = PurePosixPath(clean[len(VIEWER_DIST):]).parts
                ok = parts and all(p not in ("", ".", "..") for p in parts)
                result = viewer_dist.joinpath(*parts) if ok else viewer_dist / "__rejected__"
            else:
                result = Path(super().translate_path(path))
            if result.is_file():
                served.append(result.resolve())
            return str(result)

        def send_error(self, code, message=None, explain=None):
            http_errors.append(f"{code} {self.path.split('?', 1)[0]}")
            super().send_error(code, message, explain)

        def end_headers(self):
            self.send_header("Cache-Control", "no-store")
            super().end_headers()

        def log_message(self, *_args):
            pass

    return ThreadingHTTPServer(("127.0.0.1", 0), partial(Handler, directory=str(root)))


def static_scan(root):
    """Runtime, authoring and geometry-binding sources must not import the renderer or viewer."""
    trees = [root / "integrations/web-procedural/runtime/src", root / "packages/matter-authoring",
             root / "bindings/geometry-c/wasm"]
    scanned, offenders = [], []
    for tree in trees:
        if not tree.is_dir():
            raise AssertionError(f"static scan root missing: {tree}")
        for path in sorted(tree.rglob("*")):
            if path.suffix not in (".mjs", ".mts", ".js", ".ts", ".cjs", ".json") or "node_modules" in path.parts:
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            rel = str(path.relative_to(root))
            scanned.append(rel)
            for spec in IMPORT_RE.findall(text):
                if spec == "three" or spec.startswith("three/") or "/viewer/" in spec or spec.startswith("viewer") or "vendor/three" in spec:
                    offenders.append({"file": rel, "import": spec})
            if path.name == "package.json" and re.search(r'"three"\s*:', text):
                offenders.append({"file": rel, "import": "package.json dependency on three"})
    if offenders:
        raise AssertionError(f"renderer imports in runtime/authoring sources: {offenders}")
    return {"roots": [str(t.relative_to(root)) for t in trees], "files": len(scanned), "offenders": []}


def runtime_program_files(root, tsc):
    """Every file the runtime's own compilation pulls in; none may be viewer or three.js code."""
    tsconfig = root / "integrations/web-procedural/runtime/tsconfig.json"
    out = subprocess.run([tsc, "-p", str(tsconfig), "--listFilesOnly"], capture_output=True, text=True, check=True).stdout
    files = [line.strip() for line in out.splitlines() if line.strip()]
    project = [f for f in files if "/typescript/lib/" not in f]
    bad = [f for f in files if "/viewer/" in f or "three" in PurePosixPath(f).name.lower()]
    if bad or not project:
        raise AssertionError(f"runtime compilation includes viewer/renderer files: {bad or 'no project files listed'}")
    return {"command": f"tsc -p {tsconfig.relative_to(root)} --listFilesOnly", "project_files": [
        str(Path(f).resolve().relative_to(root)) if Path(f).resolve().is_relative_to(root) else f for f in project],
        "lib_files": len(files) - len(project)}


def vendor_check(viewer):
    manifest = json.loads((viewer / "vendor/three/MANIFEST.json").read_text())
    for name, digest in manifest["files"].items():
        actual = sha256((viewer / "vendor/three" / name).read_bytes())
        if actual != digest:
            raise AssertionError(f"vendor/three/{name} sha256 {actual} != manifest {digest}")
    return {"package": manifest["package"], "version": manifest["version"], "files": manifest["files"]}


DEMO_STEP = """async ([action, ...args]) => {
  const app = window.matterDemo;
  const outcome = await app[action](...args);
  await app.idle();
  app.render();
  const q = s => document.querySelector(s);
  const canvas = app.renderer.domElement;
  // Coverage: fraction of canvas pixels that differ from the background corner.
  const probe = Object.assign(document.createElement('canvas'), {width: 128, height: 96});
  const ctx = probe.getContext('2d');
  ctx.drawImage(canvas, 0, 0, probe.width, probe.height);
  const data = ctx.getImageData(0, 0, probe.width, probe.height).data;
  let covered = 0;
  for (let i = 0; i < data.length; i += 4)
    if (Math.abs(data[i] - data[0]) + Math.abs(data[i + 1] - data[1]) + Math.abs(data[i + 2] - data[2]) > 24) covered++;
  const info = app.renderer.info;
  return {
    status: outcome.status, epoch: outcome.epoch, error: outcome.error ? {stage: outcome.error.stage, code: outcome.error.code, message: outcome.error.message} : null,
    stateFixture: app.state.fixture.id, parameters: app.state.parameters, seed: app.state.seed.toString(),
    statusKind: q('#status').dataset.kind, statusText: q('#status').textContent,
    errorHidden: q('#error').hidden, errorCode: q('#error-code')?.textContent ?? null,
    errorStage: q('#error-stage')?.textContent ?? null, errorKept: q('#error-kept')?.textContent ?? null,
    diagnostics: [...document.querySelectorAll('#diagnostics li')].map(li => ({severity: li.dataset.severity ?? null, code: li.dataset.code, text: li.textContent})),
    stats: Object.fromEntries([...document.querySelectorAll('#stats tr[data-stat]')].map(tr => [tr.dataset.stat, tr.querySelector('td').textContent])),
    gpu: {geometries: info.memory.geometries, textures: info.memory.textures, programs: info.programs?.length ?? 0},
    viewStats: app.session.view?.stats ?? null, coverage: covered / (probe.width * probe.height),
  };
}"""

DOM_PARAMETER = """async ([name, value]) => {
  const input = document.querySelector(`#param-${name}`);
  if (input.type === 'checkbox') input.checked = value; else input.value = String(value);
  input.dispatchEvent(new Event('change'));
  await window.matterDemo.idle();
  return {parameters: window.matterDemo.state.parameters, status: document.querySelector('#status').dataset.kind,
    instances: window.matterDemo.session.view?.stats.instances ?? null};
}"""


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def run_demo(page, base, shots_dir, evidence):
    demo = {"fixtures": {}, "screenshots": []}
    page.set_viewport_size({"width": 1280, "height": 860})
    page.goto(f"{base}/integrations/web-procedural/viewer/demo/index.html?seed=1")
    page.wait_for_function("window.matterDemo || window.matterDemoFailure", timeout=60000)
    failure = page.evaluate("window.matterDemoFailure ?? null")
    check(not failure, f"demo failed to start: {failure}")
    page.evaluate("window.matterDemo.idle()")
    check(page.evaluate("document.querySelector('#status').dataset.kind") == "ok", "initial generation did not succeed")
    capabilities = page.evaluate("""() => ({
      rows: [...document.querySelectorAll('#capabilities tbody tr')].map(tr => ({support: tr.dataset.support, name: tr.cells[0].textContent})),
      native: [...document.querySelectorAll('#native-only tbody tr')].map(tr => ({badge: tr.querySelector('.badge.native')?.textContent ?? null, feature: tr.cells[0].textContent}))})""")
    check(len(capabilities["rows"]) == 8 and len(capabilities["native"]) == 8, f"capability tables: {capabilities}")
    check(all(r["badge"] == "native only" for r in capabilities["native"]), "native-only rows must carry the label")
    check({r["support"] for r in capabilities["rows"]} == {"supported", "fallback", "unsupported"}, f"capability support values {capabilities['rows']}")
    demo["capabilities"] = capabilities

    fixtures = page.evaluate("window.matterDemo.fixtures.map(f => ({id: f.id, expectError: f.expectError ?? null}))")
    for f in fixtures:
        step = page.evaluate(DEMO_STEP, ["select", f["id"]])
        expected = f["expectError"]
        if expected:
            check(step["status"] == "error" and step["error"]["stage"] == expected["stage"] and step["error"]["code"] == expected["code"],
                  f"{f['id']}: {step['error']}")
            check(not step["errorHidden"] and step["errorCode"] == expected["code"] and step["errorStage"] == f"{expected['stage']} stage",
                  f"{f['id']}: error panel {step['errorCode']} / {step['errorStage']}")
            check(step["errorKept"].startswith("Showing the last good result"), f"{f['id']}: {step['errorKept']}")
            check(step["statusKind"] == "error", f"{f['id']}: status {step['statusKind']}")
        else:
            check(step["status"] == "ok" and step["statusKind"] == "ok" and step["errorHidden"], f"{f['id']}: {step['error']}")
            check(step["coverage"] > 0.01, f"{f['id']}: canvas looks empty (coverage {step['coverage']})")
        png = page.locator("#canvas").screenshot()
        shot = {"fixture": f["id"], "sha256": sha256(png), "bytes": len(png)}
        if shots_dir:
            shots_dir.mkdir(parents=True, exist_ok=True)
            (shots_dir / f"{f['id']}.png").write_bytes(png)
            shot["file"] = f"{shots_dir.name}/{f['id']}.png"
        demo["screenshots"].append(shot)
        demo["fixtures"][f["id"]] = {k: step[k] for k in ("status", "error", "statusText", "errorKept", "diagnostics", "stats", "gpu", "coverage")}

    # Contract fixture: labeled fallbacks, ignored optional extension, attachment anchor, reject policy.
    contract = page.evaluate(DEMO_STEP, ["select", "WEB-VIEW-CONTRACT"])
    diag = contract["diagnostics"]
    fallbacks = sorted(d["text"] for d in diag if d["code"] == "fallback" and d["severity"] == "warning")
    check(len(fallbacks) == 2 and any("lod" in t for t in fallbacks) and any("collision" in t for t in fallbacks), f"fallback labels {diag}")
    check(any(d["code"] == "ignored" and d["severity"] == "info" and "demo.sparkle" in d["text"] for d in diag), f"ignored extension {diag}")
    check(any(d["code"] == "attachment" and "socket.top at (3.000, 5.000, 1.000)" in d["text"] for d in diag), f"attachment anchor {diag}")
    rejected = page.evaluate(DEMO_STEP, ["setFallbacks", "reject"])
    check(rejected["status"] == "error" and rejected["error"]["stage"] == "adapter" and rejected["error"]["code"] == "unsupported_capability",
          f"reject policy {rejected['error']}")
    kept = f"Showing the last good result (WEB-VIEW-CONTRACT, generation {contract['epoch']})."
    check(rejected["errorKept"] == kept and page.evaluate("window.matterDemo.session.view !== null"), f"kept text {rejected['errorKept']} != {kept}")
    restored = page.evaluate(DEMO_STEP, ["setFallbacks", "label"])
    check(restored["status"] == "ok", "label policy restores the contract view")
    demo["contract"] = {"diagnostics": diag, "rejected": rejected["error"], "kept": rejected["errorKept"]}

    # Parameters through the DOM controls, then seeds.
    before = page.evaluate(DEMO_STEP, ["select", "WEB-VIEW-ASSEMBLY"])["viewStats"]["instances"]
    dom = page.evaluate(DOM_PARAMETER, ["count", 7])
    # count is the number of ring placements; 12 -> 7 removes exactly five instances.
    check(dom["parameters"]["count"] == 7 and dom["status"] == "ok" and dom["instances"] == before - 5, f"DOM parameter change {dom} (before {before})")
    dom_bool = page.evaluate(DOM_PARAMETER, ["mirror", False])
    check(dom_bool["parameters"]["mirror"] is False and dom_bool["status"] == "ok", f"DOM checkbox change {dom_bool}")
    bad_seed = page.evaluate(DEMO_STEP, ["setSeed", "abc"])
    check(bad_seed["status"] == "error" and bad_seed["error"]["stage"] == "input" and bad_seed["error"]["code"] == "invalid_input"
          and bad_seed["errorCode"] == "invalid_input" and bad_seed["seed"] == "1", f"bad seed {bad_seed['error']}")
    hex_seed = page.evaluate(DEMO_STEP, ["setSeed", "0x10"])
    check(hex_seed["status"] == "ok" and hex_seed["seed"] == "16" and page.evaluate("document.querySelector('#seed').value") == "16", "hex seed")
    demo["inputs"] = {"dom_parameter": dom, "dom_checkbox": dom_bool, "bad_seed": bad_seed["error"], "hex_seed": hex_seed["seed"]}

    # Repeated updates through the UI: GPU memory is the same whenever a configuration recurs.
    configs = [(["select", "WEB-VIEW-PROP"], "WEB-VIEW-PROP"), (["set", "sides", 9], "WEB-VIEW-PROP sides=9"),
               (["select", "WEB-VIEW-CSG"], "WEB-VIEW-CSG"), (["select", "WEB-VIEW-ASSEMBLY"], "WEB-VIEW-ASSEMBLY")]
    seen, repeats = {}, []
    for i in range(16):
        call, key = configs[i % len(configs)]
        step = page.evaluate(DEMO_STEP, call)
        check(step["status"] == "ok", f"repeat {i} {key}: {step['error']}")
        repeats.append({"configuration": key, "gpu": step["gpu"]})
        if key in seen:
            check(step["gpu"] == seen[key], f"repeat {i} {key}: GPU memory {step['gpu']} != {seen[key]}")
        else:
            seen[key] = step["gpu"]
    demo["repeats"] = repeats
    final = page.evaluate("""() => { const app = window.matterDemo; app.dispose(); app.dispose();
      return {view: app.session.view, disposed: app.session.disposed, geometries: app.renderer.info.memory.geometries, textures: app.renderer.info.memory.textures}; }""")
    # three r186 keeps one module-global DFG LUT texture (created on the first PBR draw,
    # never released by three); viewer_tests' GL-object accounting pins that down.
    check(final == {"view": None, "disposed": True, "geometries": 0, "textures": THREE_GLOBAL_TEXTURES}, f"demo dispose {final}")
    demo["disposed"] = final
    evidence["demo"] = demo


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wasm-module", type=Path, required=True)
    parser.add_argument("--runtime-dir", type=Path, help="compiled runtime (default runtime/dist)")
    parser.add_argument("--viewer-dir", type=Path, help="compiled viewer (default viewer/dist)")
    parser.add_argument("--browser", type=Path, default=Path("/opt/google/chrome/chrome"))
    parser.add_argument("--tsc", default=shutil.which("tsc"), help="TypeScript compiler used to list the runtime's program files")
    parser.add_argument("--output", type=Path, help="evidence JSON (required unless --serve)")
    parser.add_argument("--screenshots", type=Path, help="directory for demo canvas screenshots")
    parser.add_argument("--serve", action="store_true", help="only serve the demo for manual use and print its URL")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[4]
    viewer = Path(__file__).resolve().parents[1]
    runtime_dist = (args.runtime_dir or viewer.parent / "runtime/dist").resolve()
    viewer_dist = (args.viewer_dir or viewer / "dist").resolve()
    module = args.wasm_module.resolve()
    if not module.is_file() or not module.with_suffix(".wasm").is_file():
        parser.error("the real WASM module and its .wasm artifact are required")
    if not (runtime_dist / "index.mjs").is_file() or not (viewer_dist / "demo/app.mjs").is_file():
        parser.error("compiled runtime and viewer are required (see README)")
    served, http_errors = [], []
    server = make_server(root, module, runtime_dist, viewer_dist, served, http_errors)
    base = f"http://127.0.0.1:{server.server_port}"
    if args.serve:
        print(f"Demo: {base}/integrations/web-procedural/viewer/demo/index.html", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass
        return 0
    if not args.output or not args.browser.is_file() or not args.tsc:
        parser.error("--output, a Chromium executable and tsc are required to run the fixtures")
    from playwright.sync_api import sync_playwright
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    evidence = {"fixtures": None, "errors": [], "console": [], "http_errors": http_errors}
    try:
        evidence["vendor"] = vendor_check(viewer)
        evidence["static_scan"] = static_scan(root)
        evidence["runtime_program"] = runtime_program_files(root, args.tsc)
        with sync_playwright() as playwright:
            browser = playwright.chromium.launch(executable_path=str(args.browser), headless=True, args=CHROME_ARGS)
            evidence["environment"] = {"browser": browser.version, "executable": str(args.browser), "chrome_args": CHROME_ARGS,
                "python": sys.version.split()[0], "platform": platform.platform()}

            def new_page():
                context = browser.new_context()
                page = context.new_page()
                page.on("pageerror", lambda error: evidence["errors"].append(str(error)))
                page.on("console", lambda msg: evidence["console"].append(f"{msg.type}: {msg.text}") if msg.type in ("error", "warning") else None)
                return context, page

            # 1. Runtime-only consumer: must not fetch the viewer or three.js.
            context, page = new_page()
            served.clear()
            page.goto(f"{base}/integrations/web-procedural/viewer/tests/runtime_only.html")
            page.wait_for_function("window.runtimeOnlyReport || window.runtimeOnlyFailure", timeout=60000)
            failure = page.evaluate("window.runtimeOnlyFailure ?? null")
            check(not failure, f"runtime-only page: {failure}")
            report = page.evaluate("window.runtimeOnlyReport")
            context.close()
            files = sorted({str(p.relative_to(root)) if p.is_relative_to(root) else str(p) for p in served})
            leaked = [f for f in files if (f.startswith("integrations/web-procedural/viewer/") and not f.endswith("tests/runtime_only.html"))
                      or "vendor/three" in f or Path(f).is_relative_to(viewer_dist)]
            check(not leaked and not report["three"], f"runtime-only build fetched viewer/renderer files: {leaked}")
            evidence["runtime_only"] = {"report": report, "served": files}

            # 2. Adapter and end-to-end fixtures.
            context, page = new_page()
            page.goto(f"{base}/integrations/web-procedural/viewer/tests/viewer_tests.html")
            page.wait_for_function("window.viewerReport || window.viewerFailure", timeout=300000)
            failure = page.evaluate("window.viewerFailure ?? null")
            if failure:
                evidence["failure"] = failure
                raise AssertionError(failure)
            evidence["fixtures"] = page.evaluate("window.viewerReport")
            context.close()

            # 3. The demo UI.
            context, page = new_page()
            run_demo(page, base, args.screenshots.resolve() if args.screenshots else None, evidence)
            context.close()
            browser.close()
        if evidence["errors"]:
            raise AssertionError(evidence["errors"])
        # The pages declare an inline icon, so no fixture or page may request a missing file.
        check(not http_errors, f"HTTP errors while serving the fixtures: {http_errors}")
        evidence["result"] = "pass"
        print("PASS:", ", ".join(evidence["fixtures"]["passed"]), "+ DEMO-UI, RUNTIME-ONLY, STATIC-SCAN, VENDOR")
        print("Evidence:", args.output)
        return 0
    except BaseException as error:
        evidence["result"] = f"fail: {error}"
        raise
    finally:
        server.shutdown()
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(evidence, indent=2, default=str) + "\n")


if __name__ == "__main__":
    raise SystemExit(main())
