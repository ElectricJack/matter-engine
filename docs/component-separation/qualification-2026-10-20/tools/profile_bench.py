"""validation.md provisional-profile protocol on the parity step
protocol. Per case and backend: one fresh process, create, 1 cold evaluate,
5 warmups, 30 measured evaluate+release pairs; per-step pipe round-trip time.
Usage: profile_bench.py <parity-out/raw> <matter_parity_native> <node> <node_wasm_stream.mjs> <wasm .mjs> <out.json>"""
import json, pathlib, statistics, subprocess, sys, time
raw, runner, nodejs, driver, module, out = map(pathlib.Path, sys.argv[1:7])
PROFILES = {'props-small': [('PROP-SLAB', 'G-MATERIAL'), ('PROP-FLOWER', 'G-SEEDED-VARIATION'), ('GEOM-EXTRUDE', 'G-EXTRUDE')],
            'solid-source-csg': [('GEOM-CSG', 'G-ORDERED-CSG')],
            'assembly-shared': [('BUILD-SHARED', 'G-SHARED-INSTANCES')],
            '(primitive reference)': [('CORPUS-PRIMITIVE', 'G-PRIMITIVE')]}
def first_eval(path):
    for line in path.read_text().splitlines():
        s = json.loads(line)
        if s['op'] == 'evaluate': return s
def pct(v, p): v = sorted(v); return v[min(len(v) - 1, round(p / 100 * (len(v) - 1)))]
rows = []
for profile, cases in PROFILES.items():
    for fixture, case in cases:
        for backend in ('native', 'node-wasm'):
            step = first_eval(raw / f"{case}.{'native' if backend == 'native' else 'wasm'}.in.jsonl")
            cmd = [str(runner)] if backend == 'native' else [str(nodejs), str(driver), str(module)]
            t0 = time.perf_counter()
            p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
            if backend != 'native': assert json.loads(p.stdout.readline())['ready'] == 1
            def call(s):
                t = time.perf_counter(); p.stdin.write(json.dumps(s) + '\n'); p.stdin.flush()
                r = json.loads(p.stdout.readline()); return r, (time.perf_counter() - t) * 1000
            r, _ = call({'op': 'create', 'instance': step['instance']}); assert r['status'] == 0, r
            startup = (time.perf_counter() - t0) * 1000
            times = []; wire = None
            for i in range(36):
                s = dict(step, **{'as': f'r{i}'}); r, dt = call(s); assert r['status'] == 0, (case, backend, r)
                wire = r.get('wire_bytes'); times.append(dt)
                r, _ = call({'op': 'release', 'instance': step['instance'], 'result': f'r{i}'}); assert r['status'] == 0, r
            p.stdin.close(); p.wait()
            m = times[6:]
            rows.append(dict(profile=profile, fixture=fixture, case=case, backend=backend, wire_bytes=wire,
                             startup_to_instance_ms=round(startup, 2), cold_eval_ms=round(times[0], 3),
                             median_ms=round(statistics.median(m), 3), p95_ms=round(pct(m, 95), 3), max_ms=round(max(m), 3),
                             samples_ms=[round(x, 3) for x in m]))
            print({k: v for k, v in rows[-1].items() if k != 'samples_ms'}, flush=True)
out.write_text(json.dumps(dict(protocol='1 process per case/backend; create; 1 cold + 5 warmup + 30 measured evaluate (each released); '
    'time = stdin->stdout pipe round-trip of the evaluate step, which includes reading the result wire', rows=rows), indent=1))
