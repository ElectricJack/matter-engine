#!/usr/bin/env python3
"""Native/browser pixel-matched view of the graphics authoring fixtures.

Native: MATTER_AUTHORING_E2E_DUMP=<dir> matter_authoring_e2e_tests graphics
writes <dir>/<generator>.native.json (VulkanRaster albedo, world normal,
reversed depth and instance identity under the e2e's orthographic camera).
Browser: browser_matched.html runs the same generators and seed through the
worker runtime and the viewer's AssetView in real Chromium (SwiftShader
WebGL2, as in the viewer fixtures) and rasterizes them with the same camera,
supersampled by an odd --scale and read at each pixel's centre sample so
SwiftShader's 4 subpixel bits do not stand in for the viewer (see the .mjs).
Per pixel: identical instance coverage (<= 0.5% mismatched), and on pixels
both cover with the same instance, albedo and encoded normal (n*0.5+0.5)
within 2/255 and depth within 1e-4, each failing on <= 0.5% of an instance's
pixels (the matched_compare.py tolerances).

usage: browser_matched.py --wasm-module <matter_geometry_wasm.mjs> --native <dir> [--output report.json] [--dump dir]
"""
import argparse
import json
from pathlib import Path
import sys
import threading

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "integrations/web-procedural/viewer/tests"))
from browser_tests import CHROME_ARGS, make_server  # noqa: E402

GENERATORS = ["primitive", "materialAssignment", "orderedCsg", "sharedInstances"]
TOL, MAXPCT, DEPTH_TOL = 2 / 255, 0.5, 1e-4


def compare(native, browser):
    n = native["width"] * native["height"]
    assert (browser["width"], browser["height"]) == (native["width"], native["height"])
    nid = [native["instances"][k] if k >= 0 else None for k in native["identity"]]
    bid = browser["identity"]
    cov = dict(both=0, native_only=0, browser_only=0, instance_mismatch=0)
    per = {}
    for i in range(n):
        a, b = nid[i], bid[i]
        if a is None and b is None:
            continue
        if b is None:
            cov["native_only"] += 1
            continue
        if a is None:
            cov["browser_only"] += 1
            continue
        if a != b:
            cov["instance_mismatch"] += 1
            continue
        cov["both"] += 1
        s = per.setdefault(a, dict(px=0, albedo_max=0.0, normal_max=0.0, depth_max=0.0, albedo_bad=0, normal_bad=0, depth_bad=0))
        s["px"] += 1
        ad = max(abs(native["albedo"][3 * i + c] - browser["albedo"][3 * i + c]) for c in range(3))
        nd = max(abs(native["normal"][3 * i + c] - browser["normal"][3 * i + c]) * 0.5 for c in range(3))
        dd = abs(native["depth"][i] - browser["depth"][i])
        for key, value, tol in (("albedo", ad, TOL), ("normal", nd, TOL), ("depth", dd, DEPTH_TOL)):
            s[f"{key}_max"] = max(s[f"{key}_max"], value)
            s[f"{key}_bad"] += value > tol
    covered = max(cov["both"] + cov["native_only"] + cov["browser_only"] + cov["instance_mismatch"], 1)
    mismatch_pct = 100 * (cov["native_only"] + cov["browser_only"] + cov["instance_mismatch"]) / covered
    ok = mismatch_pct <= MAXPCT and set(per) == {x for x in nid if x is not None}
    for s in per.values():
        for key in ("albedo", "normal", "depth"):
            s[f"{key}_bad_pct"] = round(100 * s[f"{key}_bad"] / s["px"], 3)
            ok &= s[f"{key}_bad_pct"] <= MAXPCT
    return dict(ok=ok, coverage=cov, coverage_mismatch_pct=round(mismatch_pct, 3), instances=per,
                native_instances=native["instances"], browser_instances=browser["instances"])


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--wasm-module", type=Path, required=True)
    parser.add_argument("--native", type=Path, required=True, help="MATTER_AUTHORING_E2E_DUMP directory")
    parser.add_argument("--browser", type=Path, default=Path("/opt/google/chrome/chrome"))
    parser.add_argument("--output", type=Path, help="write the full report JSON here")
    parser.add_argument("--scale", type=int, default=17, help="odd browser supersample; output pixels read at their centre sample (1 = plain render)")
    parser.add_argument("--dump", type=Path, help="write <generator>.browser.json buffers (native dump layout) here")
    args = parser.parse_args()
    module = args.wasm_module.resolve()
    runtime_dist = ROOT / "integrations/web-procedural/runtime/dist"
    viewer_dist = ROOT / "integrations/web-procedural/viewer/dist"
    if not module.is_file() or not module.with_suffix(".wasm").is_file():
        parser.error("the real WASM module and its .wasm artifact are required")
    if not (runtime_dist / "index.mjs").is_file() or not (viewer_dist / "index.mjs").is_file():
        parser.error("compiled runtime and viewer are required (tsc -p their tsconfig.json)")
    natives = {g: json.loads((args.native / f"{g}.native.json").read_text()) for g in GENERATORS}
    served, http_errors = [], []
    server = make_server(ROOT, module, runtime_dist, viewer_dist, served, http_errors)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    page_url = (f"http://127.0.0.1:{server.server_port}/docs/component-separation/qualification-2026-10-20/"
                f"tools/browser_matched.html?generators={','.join(GENERATORS)}&scale={args.scale}")
    from playwright.sync_api import sync_playwright
    errors = []
    with sync_playwright() as playwright:
        browser = playwright.chromium.launch(executable_path=str(args.browser), headless=True, args=CHROME_ARGS)
        page = browser.new_page()
        page.on("pageerror", lambda error: errors.append(str(error)))
        page.on("console", lambda msg: errors.append(f"{msg.type}: {msg.text}") if msg.type == "error" else None)
        page.goto(page_url)
        page.wait_for_function("window.matchedReport || window.matchedFailure", timeout=300000)
        failure = page.evaluate("window.matchedFailure ?? null")
        report = None if failure else page.evaluate("window.matchedReport")
        version = browser.version
        browser.close()
    server.shutdown()
    if failure or errors or http_errors:
        print("FAIL browser page:", failure, errors, http_errors)
        return 1
    if args.dump:
        args.dump.mkdir(parents=True, exist_ok=True)
        for g in GENERATORS:
            (args.dump / f"{g}.browser.json").write_text(json.dumps(report[g]))
    results = {g: compare(natives[g], report[g]) for g in GENERATORS}
    env = report["environment"]
    print(f"browser: Chromium {version}, {env['renderer']}, SUBPIXEL_BITS {env['subpixel_bits']}, scale {env['scale']}, three r{env['three']}")
    for g, r in results.items():
        print(f"{g}: {'PASS' if r['ok'] else 'FAIL'} coverage {r['coverage']} mismatch {r['coverage_mismatch_pct']}%")
        for inst, s in r["instances"].items():
            print(f"  {inst}: px={s['px']} albedo max {s['albedo_max']:.6f} ({s['albedo_bad_pct']}% > 2/255) "
                  f"normal max {s['normal_max']:.6f} ({s['normal_bad_pct']}%) depth max {s['depth_max']:.2e} ({s['depth_bad_pct']}%)")
        if not r["ok"]:
            print(f"  native instances {r['native_instances']} browser instances {r['browser_instances']}")
    if args.output:
        args.output.write_text(json.dumps(dict(environment=dict(chromium=version, **report["environment"]), results=results), indent=1))
    ok = all(r["ok"] for r in results.values())
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
