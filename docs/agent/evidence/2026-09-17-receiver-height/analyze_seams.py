"""Describe raw seam controls; these statistics do not establish visual acceptance."""
import hashlib
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image

folder = Path(sys.argv[1])
palette = {
    'hit': (0, 255, 0), 'connected_hit': (255, 255, 0),
    'initial_boundary': (255, 128, 0), 'path_boundary': (255, 0, 0),
    'snapshot_mismatch': (255, 0, 255), 'travel_limit': (0, 0, 255),
    'unresolved': (0, 255, 255), 'unsupported': (255, 255, 255),
    'zero_depth': (0, 64, 0), 'off': (64, 64, 64),
}
result = {'limits': 'Fixed cameras only. POM changes sample positions, so on/off pixel differences are expected. Chart colors alias every 16 IDs; detected color boundaries are a subset. Edge statistics describe correlation, not the cause of an artifact.',
          'views': {}, 'inputs': {}}

def read(name):
    path = folder / name
    result['inputs'][name] = hashlib.sha256(path.read_bytes()).hexdigest()
    return np.asarray(Image.open(path).convert('RGB')).astype(np.int16)

def edges(image):
    out = np.zeros(image.shape[:2], dtype=np.int16)
    out[:, 1:] = np.max(np.abs(image[:, 1:] - image[:, :-1]), axis=2)
    out[1:, :] = np.maximum(out[1:, :], np.max(np.abs(image[1:, :] - image[:-1, :]), axis=2))
    return out

def describe(values):
    return {'pixels': int(values.size), 'mean_max_channel_jump': float(values.mean()),
            'p95_max_channel_jump': float(np.percentile(values, 95))} if values.size else None

for view in ('cliff', 'close'):
    row = {}
    paths = read(f'{view}-seam-path.png')
    classified = np.zeros(paths.shape[:2], dtype=bool)
    row['path_pixels'] = {}
    for name, color in palette.items():
        mask = np.max(np.abs(paths - np.array(color)), axis=2) <= 1
        row['path_pixels'][name] = int(mask.sum())
        classified |= mask
    row['path_pixels']['unclassified'] = int((~classified).sum())
    for channel in ('normal', 'albedo'):
        on, off = read(f'{view}-{channel}.png'), read(f'{view}-seam-{channel}-flat.png')
        difference = np.abs(on - off)
        row[channel + '_pom_difference'] = {
            'mean_absolute_byte_error': float(difference.mean()),
            'fraction_pixels_over_3_bytes': float((difference.max(axis=2) > 3).mean()),
        }
    chart = read(f'{view}-seam-chart-flat.png')
    boundary = edges(chart) > 8
    near = boundary.copy()
    for dy in range(-3, 4):
        for dx in range(-3, 4):
            shifted = np.roll(boundary, (dy, dx), axis=(0, 1))
            if dy > 0: shifted[:dy, :] = False
            elif dy < 0: shifted[dy:, :] = False
            if dx > 0: shifted[:, :dx] = False
            elif dx < 0: shifted[:, dx:] = False
            near |= shifted
    normal_edges = edges(read(f'{view}-seam-normal-flat.png'))
    failure = np.zeros(paths.shape[:2], dtype=bool)
    for status in ('initial_boundary', 'path_boundary'):
        failure |= np.max(np.abs(paths - np.array(palette[status])), axis=2) <= 1
    row['boundary_failures_near_chart_color_boundary'] = {
        'total': int(failure.sum()), 'within_3_pixels': int((failure & near).sum()),
    }
    unresolved = np.max(np.abs(paths - np.array(palette['unresolved'])), axis=2) <= 1
    row['unresolved_in_top_tenth'] = int(unresolved[:paths.shape[0] // 10].sum())
    row['normal_flat_near_chart_color_boundary'] = describe(normal_edges[near])
    row['normal_flat_away_from_chart_color_boundary'] = describe(normal_edges[~near])
    result['views'][view] = row

(folder / 'seam-analysis.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result['views'], indent=2))
