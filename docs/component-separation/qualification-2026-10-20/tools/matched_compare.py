"""Per-pixel matched-view compare: portable VulkanRaster capture vs the legacy
renderer's debug views of the same world/camera/size.
Usage: matched_compare.py <rundir from run_matched_view.sh>"""
import sys, json, math, struct
from PIL import Image
d = sys.argv[1]; TOL = 2; MAXPCT = 0.5
def pfm(p):
    b = open(p, 'rb').read(); parts = b.split(b'\n', 3)
    ch = 3 if parts[0] == b'PF' else 1; w, h = map(int, parts[1].split()); scale = float(parts[2])
    data = struct.unpack(('<' if scale < 0 else '>') + 'f' * (w * h * ch), parts[3][:w * h * ch * 4])
    rows = [data[(h - 1 - y) * w * ch:(h - y) * w * ch] for y in range(h)]  # bottom-up -> top-down
    return w, h, ch, [tuple(r[x * ch:(x + 1) * ch]) for r in rows for x in range(w)]
w, h, _, ident = pfm(d + '/portable.identity.pfm')
_, _, _, normal = pfm(d + '/portable.normal.pfm')
_, _, _, depth = pfm(d + '/portable.depth.pfm')
cap = json.load(open(d + '/portable.json'))
la = list(Image.open(d + '/legacy_albedo.png').convert('RGB').get_flattened_data())
ln = list(Image.open(d + '/legacy_normal.png').convert('RGB').get_flattened_data())
ld = list(Image.open(d + '/legacy_depth.png').convert('RGB').get_flattened_data())
pa = list(Image.open(d + '/portable.albedo.png').convert('RGB').get_flattened_data())
CRATE = (209, 199, 184)  # legacy palette albedo of the crate material
NEAR, FAR, EV = 0.1, 10241.0, -2.0  # MatterEditor/src/main.cpp:685-690 camera; display exposure default
def aces(v):
    x = max(v * 2 ** EV, 0.0); return min(max(x * (2.51 * x + 0.03) / max(x * (2.43 * x + 0.59) + 0.14, 1e-6), 0.0), 1.0)
def oetf(c): return c * 12.92 if c <= 0.0031308 else 1.055 * c ** (1 / 2.4) - 0.055
def enc_normal(n): return tuple(round(oetf(aces(c * 0.5 + 0.5)) * 255) for c in n)
def enc_depth(hw):
    lin = NEAR * FAR / max(hw * (FAR - NEAR) + NEAR, 1e-6)
    t = math.log(min(max(lin, 1.0), 8192.0)) / math.log(8192.0)
    return tuple(round(v * 255) for v in (t, (t * 32) % 1.0, (t * 32 + 0.5) % 1.0))
def cdiff(a, b, wrap=False):
    return max(min(abs(x - y), 256 - abs(x - y)) if (wrap and i) else abs(x - y) for i, (x, y) in enumerate(zip(a, b)))
n_draws = len(cap['draws'])
stats = {i: dict(legacy_farther=0, px=0, cov_mismatch=0, normal_bad=0, depth_bad=0, normal_max=0, depth_max=0, albedo_bad=0, albedo_max=0) for i in range(n_draws)}
cov = dict(portable_only=0, legacy_only=0, both=0)
for i in range(w * h):
    k = int(ident[i][1]); lc = la[i] == CRATE
    if k < 0:
        if lc: cov['legacy_only'] += 1
        continue
    if not lc: cov['portable_only'] += 1; stats[k]['cov_mismatch'] += 1
    else: cov['both'] += 1
    s = stats[k]; s['px'] += 1
    nd = cdiff(enc_normal(normal[i]), ln[i]); s['normal_max'] = max(s['normal_max'], nd); s['normal_bad'] += nd > TOL
    ad = cdiff(pa[i], la[i]); s['albedo_max'] = max(s['albedo_max'], ad); s['albedo_bad'] += ad > TOL
    lt = (ld[i][0] / 255); s['legacy_farther'] += lt > enc_depth(depth[i][0])[0] / 255 + 1 / 255
    dd = cdiff(enc_depth(depth[i][0]), ld[i], wrap=True); s['depth_max'] = max(s['depth_max'], dd); s['depth_bad'] += dd > TOL
bg_depth = {round(depth[i][0], 6) for i in range(w * h) if ident[i][1] < 0}
print('coverage', cov, 'portable background hw depth values', sorted(bg_depth)[:4])
print('portable albedo over crates', {pa[i] for i in range(w * h) if ident[i][1] >= 0})
fail = cov['portable_only'] + cov['legacy_only'] > 0
for k, s in stats.items():
    dr = cap['draws'][k]; px = max(s['px'], 1)
    nb, db = 100 * s['normal_bad'] / px, 100 * s['depth_bad'] / px
    ab = 100 * s['albedo_bad'] / px; aok = ab <= MAXPCT
    ok = s['px'] == dr['pixels'] and nb <= MAXPCT and db <= MAXPCT and aok
    # Legacy rasterizes with a fixed frontFace (vk_scene_renderer.cpp:2769) and no
    # per-instance winding parity, so a det<0 placement draws its back faces:
    # coverage must still match, depth/normal parity is not expected.
    if dr['mirrored'] and not ok and s['px'] == dr['pixels'] and aok:
        print(f"  legacy-known: mirrored {dr['instance']} legacy farther than portable at {s['legacy_farther']}/{s['px']} px (back faces)")
        ok = 'LEGACY-INSIDE-OUT'
    fail |= not ok
    print(f"{dr['instance']} mirrored={dr['mirrored']} px={s['px']} (json {dr['pixels']}) "
          f"normal>tol {s['normal_bad']} ({nb:.2f}%, max {s['normal_max']}) depth>tol {s['depth_bad']} ({db:.2f}%, max {s['depth_max']}) albedo>tol {s['albedo_bad']} ({ab:.2f}%, max {s['albedo_max']}) {ok if isinstance(ok, str) else ('PASS' if ok else 'FAIL')}")
print('RESULT', 'FAIL' if fail else 'PASS', f'(channel-tol {TOL}, max-diff-pct {MAXPCT})')
if len(sys.argv) > 2:
    seen = set()
    for i in range(w * h):
        k = int(ident[i][1])
        if k >= 0 and k not in seen and (i % w) % 7 == 0:
            seen.add(k); print(k, 'hw', depth[i][0], 'pred', enc_depth(depth[i][0]), 'legacy', ld[i], 'n', [round(c, 3) for c in normal[i]], enc_normal(normal[i]), ln[i])
