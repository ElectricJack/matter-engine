"""Summarize accepted native logs and retain their raw GPU channel evidence."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import zipfile

parser = argparse.ArgumentParser()
parser.add_argument('raw', type=Path)
args = parser.parse_args()
out = Path(__file__).resolve().parent
repo = out.parents[3]
prefix = 'finite-stamp-v5'
text = (out / f'{prefix}-projection-clay.log').read_text()
stamps = [dict(samples=int(n), levels=int(l), pixels=int(p), error=float(e), height=float(h), detail=float(d))
          for n, l, p, e, h, d in re.findall(r'STAMP oracle samples=(\d+) levels=(\d+) pixels=(\d+) max_error=(\S+) height_error_m=(\S+) detail_error_m=(\S+)', text)]
materials = [dict(exact=int(a), channels=float(c), height=float(h), normal=float(n), coverage=int(v))
             for a, c, h, n, v in re.findall(r'MATERIAL oracle exact=(\d+) channels=(\S+) microheight_m=(\S+) normal=(\S+) coverage=(\d+)', text)]
assert len(stamps) == 48 and len(materials) == 20
for name in ['finite-stamp-v3', prefix]:
    for check in json.loads((out / f'{name}-checks.json').read_text()):
        assert check['exit'] == 0 and not check['source_changes'] and not check['validation_errors']
source = json.loads((out / f'{prefix}-source.json').read_text())

def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()

changed = [name for name, digest in source['sources'].items() if sha(repo / name) != digest]
assert not changed, changed
raw = {str(p.relative_to(args.raw)): {'sha256': sha(p), 'bytes': p.stat().st_size}
       for p in sorted(args.raw.iterdir()) if p.suffix in ['.bin', '.json']}
assert len(raw) == 204  # 48 geometry + 48 material + 6 filtered views, each with metadata
archive = out / 'raw-face-channels.zip'
with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for name in raw:
        z.write(args.raw / name, name)
summary = {
    'stamp_faces': len(stamps), 'stamp_queries': sum(x['samples'] for x in stamps),
    'stamp_max_channel_error': max(x['error'] for x in stamps),
    'stamp_max_height_error_m': max(x['height'] for x in stamps),
    'stamp_max_detail_error_m': max(x['detail'] for x in stamps),
    'stamp_all_mip_pixels': sum(x['pixels'] for x in stamps),
    'stamp_all_mip_float_bytes': sum(x['pixels'] for x in stamps) * 64,
    'largest_stamp_float_bytes': max(x['pixels'] for x in stamps) * 64,
    'material_oracles': len(materials),
    'material_cpu_max_channel_error': max(x['channels'] for x in materials),
    'material_cpu_max_height_error_m': max(x['height'] for x in materials),
    'material_cpu_max_normal_component_error': max(x['normal'] for x in materials),
    'material_total_coverage_mismatches': sum(x['coverage'] for x in materials),
    'material_exact_max_error': max(max(x['channels'], x['height'], x['normal']) for x in materials if x['exact']),
    'source_changes_after_validation': changed,
    'raw': raw,
    'archive': {'name': archive.name, 'bytes': archive.stat().st_size, 'sha256': sha(archive)},
}
(out / f'{prefix}-summary.json').write_text(json.dumps(summary, indent=2) + '\n')
print(json.dumps({k: v for k, v in summary.items() if k != 'raw'}, indent=2))
