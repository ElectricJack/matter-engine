"""Measure settled captures; terrain crops exclude moving forest and sky."""
import json
import re
from pathlib import Path
import numpy as np
from PIL import Image

root = Path(__file__).resolve().parent
audits = {label: json.loads((root / label / 'audit.json').read_text())
          for label in ('before', 'after')}
result = {'isolation': audits['after']['isolation'], 'views': {}}
for label, audit in audits.items():
    assert audit['editor_exit'] == 0 and audit['binary_unchanged']
    assert not any(audit[k] for k in ('source_changes', 'missing_shots',
                                     'validation_errors', 'command_failures'))
for view in ('overview', 'grazing'):
    roi = (128, 520, 1152, 800)
    entry = {'terrain_roi_xyxy': roi, 'images': {}, 'timings': {}, 'residency': {}}
    for label, audit in audits.items():
        entry['timings'][label] = audit['timings'][view]
        assert audit['timings'][view]['samples'] == 30
        rows = [line for line in audit['stats']
                if line.startswith('STATSVT,' + view + '-sample-')]
        assert len(rows) == 30
        states = set()
        for line in rows:
            assert all(token in line for token in
                       ('active=1,', 'queue=0,', 'rejected=0,', 'evictions=0,'))
            states.add(tuple(re.search(pattern, line)[1] for pattern in
                             (r'variants=(\d+)/', r'pool=(\d+)/', r'ind=([\d.]+)/')))
        assert len(states) == 1
        entry['residency'][label] = states.pop()
    assert entry['residency']['before'] == entry['residency']['after']
    for mode in ('lit', 'albedo', 'normal'):
        a, b = (np.asarray(Image.open(root / label / f'{view}-{mode}.png').convert('RGB'),
                           dtype=np.int16) for label in ('before', 'after'))
        assert a.shape == b.shape
        d = np.abs(a - b)
        x0, y0, x1, y1 = roi
        entry['images'][mode] = {}
        for region, values in (('full_frame', d), ('terrain', d[y0:y1, x0:x1])):
            entry['images'][mode][region] = {
                'mean_abs_rgb_255': float(values.mean()),
                'max_abs_channel_255': int(values.max()),
                'fraction_pixels_changed': float((values.max(axis=2) > 0).mean()),
                'fraction_pixels_over_3': float((values.max(axis=2) > 3).mean())}
    old = entry['timings']['before']['gbuffer_median_ms']
    new = entry['timings']['after']['gbuffer_median_ms']
    entry['median_reduction_percent'] = 100 * (1 - new / old)
    result['views'][view] = entry
(root / 'comparison.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result, indent=2))
