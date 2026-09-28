"""Report the independent-owner POM gate without treating a reproduced bug as a pass."""
import hashlib
import json
from pathlib import Path
import re
import sys

out = Path(__file__).resolve().parent
prefix = sys.argv[1]
assert re.fullmatch(r'[a-z0-9-]+', prefix)
checks = out.parent / '2026-09-16-shared-vt-pixels'
log_path = checks / f'{prefix}-test-vt-sector-seam.log'
text = log_path.read_text(errors='replace')
# Native stdout and stderr were combined by the runner. Complete diagnostic
# writes can occur inside a buffered measurement line. Remove only this exact
# diagnostic grammar, retaining every measurement byte and all strict counts.
text, diagnostics_removed = re.subn(
    r'\[vt-surface-links\] owner=\d+ target=\d+ boundaries=\d+/\d+ intervals=\d+\n', '', text)
groups = []
samples = []
for line in text.splitlines():
    if not line.startswith(('VT_SECTOR_SEAM ', 'VT_SECTOR_SAMPLE ')):
        continue
    fields = dict(item.split('=', 1) for item in line.split()[1:])
    (groups if line.startswith('VT_SECTOR_SEAM ') else samples).append(fields)
reference = [g for g in groups if g['layout'] == 'reference']
split = [g for g in groups if g['layout'] != 'reference']
check_runs = json.loads((checks / f'{prefix}-tests.json').read_text())['runs']
run = next(r for r in check_runs if r['mode'] == 'vt-sector-seam')
fixture_valid = (len(groups) == 12 and len(samples) == 120 and len(reference) == 4
    and all(float(g['flat_error']) < .0001 and int(g['crossed']) >= 3 for g in groups)
    and all(s['mip'] == '0/0' for s in samples)
    and all((len(set(g['slots'].split(','))) == 1) == (g['layout'] == 'reference') for g in groups)
    and all(float(g['raster_error']) < .0003 and float(g['rt_error']) < .0003 for g in reference)
    and run['binary_unchanged'] and not run['source_changes'] and run['zero_validation_errors'])
continuity_passed = fixture_valid and run['passed'] and all(
    float(g['raster_error']) < .0003 and float(g['rt_error']) < .0003
    and max(float(g[k]) for k in ('color_error', 'roughness_error', 'normal_error')) < .015
    for g in groups)
audit = {
    'log': str(log_path.relative_to(out.parents[3])),
    'log_sha256': hashlib.sha256(log_path.read_bytes()).hexdigest(),
    'diagnostic_records_removed_for_parsing': diagnostics_removed,
    'fixture_valid': fixture_valid,
    'continuity_passed': continuity_passed,
    'reference_max_position_error_m': max(float(g[k]) for g in reference for k in ('raster_error', 'rt_error')),
    'split_max_position_error_m': max(float(g[k]) for g in split for k in ('raster_error', 'rt_error')),
    'split_undisplaced_raster_samples': sum(int(g['flat_raster']) for g in split),
    'split_undisplaced_rt_samples': sum(int(g['flat_rt']) for g in split),
    'groups': groups,
    'limits': ['Synthetic planar receivers exercise the sector ownership contract.',
        'Unequal tessellation is not a live terrain LOD transition.',
        'Streaming, edits, eviction, real terrain and visual acceptance remain separate gates.'],
}
(out / f'{prefix}-audit.json').write_text(json.dumps(audit, indent=2) + '\n')
print(json.dumps({k: v for k, v in audit.items() if k != 'groups'}, indent=2))
raise SystemExit(0 if continuity_passed else 1)
