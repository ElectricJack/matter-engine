"""Per-case process timing for the native C-ABI runner vs Node WASM on the
parity harness's own recorded step files. Run 1 = cold, runs 2..N = warm.
Usage: measure_native.py <parity-out/raw> <matter_parity_native> <parity/node_wasm.mjs> <node> <wasm .mjs> <out.json> <runs>"""
import json, pathlib, subprocess, sys, time, statistics
raw, runner, node, nodejs, module, out = map(pathlib.Path, sys.argv[1:7]); N = int(sys.argv[7])
rows = []
for f in sorted(raw.glob('G-*.native.in.jsonl')):
    case = f.name.split('.native')[0]; stdin = f.read_bytes()
    wasm_in = raw / f'{case}.wasm.in.jsonl'
    for backend, cmd, data in (('native', [str(runner)], stdin),
                               ('node-wasm', [str(nodejs), str(node), str(module)], wasm_in.read_bytes())):
        times = []; outb = 0; rss = 0
        for i in range(N):
            t0 = time.perf_counter(); p = subprocess.run(cmd, input=data, capture_output=True); times.append((time.perf_counter() - t0) * 1000)
            tp = subprocess.run(['/usr/bin/time', '-f', '%M', *cmd], input=data, capture_output=True)
            rss = max(rss, int(tp.stderr.decode().strip().splitlines()[-1])); outb = len(p.stdout)
            assert p.returncode == 0, (case, backend, p.stderr[-400:])
        rows.append(dict(case=case, backend=backend, steps=data.count(b'\n'), in_bytes=len(data), out_bytes=outb,
                         cold_ms=round(times[0], 1), warm_median_ms=round(statistics.median(times[1:]), 1),
                         warm_min_ms=round(min(times[1:]), 1), max_rss_kib=rss))
        print(rows[-1], flush=True)
out.write_text(json.dumps(dict(runs=N, note='process wall time incl. startup; max RSS from /usr/bin/time -f %M', rows=rows), indent=1))
