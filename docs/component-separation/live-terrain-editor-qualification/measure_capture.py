"""Per-draw evidence from one MATTER_PORTABLE_SCENE_CAPTURE prefix: for each
draw in <prefix>.json, the pixels whose identity names that draw, the distinct
raster identities on them, and the depth range on exactly those pixels.
Usage: python3 -I measure_capture.py <prefix>   (stdlib only)"""
import array, json, sys

def pfm(path):
    with open(path, 'rb') as f:
        kind = f.readline().strip(); w, h = map(int, f.readline().split()); scale = float(f.readline())
        assert scale < 0, 'little-endian PFM expected'
        data = array.array('f'); data.frombytes(f.read())
    c = 3 if kind == b'PF' else 1
    assert len(data) == w * h * c, path
    return w, h, c, data

prefix = sys.argv[1]
meta = json.load(open(prefix + '.json'))
w, h, _, ident = pfm(prefix + '.identity.pfm')
_, _, _, depth = pfm(prefix + '.depth.pfm')
rows = {}
for p in range(w * h):
    draw = int(ident[p * 3 + 1])
    r = rows.setdefault(draw, {'pixels': 0, 'raster_ids': set(), 'depth': [float('inf'), float('-inf')]})
    r['pixels'] += 1; r['raster_ids'].add(int(ident[p * 3])); d = depth[p]
    r['depth'][0] = min(r['depth'][0], d); r['depth'][1] = max(r['depth'][1], d)
print(f"revision {meta['revision']} {w}x{h} draws={len(meta['draws'])} refused={meta['refused']} "
      f"refused_placements={meta['refused_placements']}")
terrain = 0
for i, d in enumerate(meta['draws']):
    r = rows.get(i, {'pixels': 0, 'raster_ids': set(), 'depth': [None, None]})
    assert r['pixels'] == d['pixels'], (i, r['pixels'], d['pixels'])  # identity image agrees with the JSON
    terrain += d['instance'].startswith('terrain.live') and r['pixels'] > 0
    print(f"draw {i:2d} {d['instance']:<28} raster={d['raster_identity']:<28} prototype={d['prototype']:<24} "
          f"pixels={r['pixels']:6d} raster_ids={sorted(r['raster_ids'])} depth=[{r['depth'][0]}, {r['depth'][1]}] "
          f"resident_bytes={d['resident_bytes']}")
bg = rows.get(-1)
if bg: print(f"background pixels={bg['pixels']} depth=[{bg['depth'][0]}, {bg['depth'][1]}]")
print(f"terrain draws with pixels: {terrain}")
