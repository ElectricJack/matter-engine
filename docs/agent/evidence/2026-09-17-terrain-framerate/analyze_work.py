"""Decode opt-in POM work captures; log bins yield count intervals, not exact counts."""
import json
from pathlib import Path
import sys

import numpy as np
from PIL import Image

root = Path(sys.argv[1])
report = {'encoding': 'round(log2(count+1)/24*255)',
          'scope': 'Visible surviving fragments only; excludes occluded fragments and diagnostic timings.',
          'encoded_tolerance': 'Rounding half-bin plus one additional byte step for the display/readback path; counts are bounded estimates.',
          'views': {}}


def bounds(raw):
    x = raw.astype(np.float64)
    # The observed byte 41 falls between ideal integer-count bins. Include
    # one additional byte step for the render/display/readback path rather
    # than claiming exact counter readback. Keep count zero exact.
    lo = np.maximum(0, np.ceil(np.exp2(np.maximum(x-1.5, 0)*24/255)-1-1e-6))
    hi = np.maximum(0, np.floor(np.exp2((x+1.5)*24/255)-1+1e-6))
    lo[x == 0] = hi[x == 0] = 0
    return lo, hi


for view in (sys.argv[2:] or ('valley', 'cliff')):
    assert view in ('valley', 'cliff')
    seed = np.array(Image.open(root / f'{view}-pom-albedo.png').convert('RGB'))
    walk = np.array(Image.open(root / f'{view}-pom-walk-work.png').convert('RGB'))
    assert seed.shape == walk.shape == (1044, 2796, 3)
    lo, hi = bounds(seed)
    # All seed tests call the projection predicate. A mismatch reveals an
    # invalid capture/encoding rather than authorizing arbitrary work claims.
    assert np.all(hi[:,:,2] >= lo[:,:,0]), 'seed tests exceed total projections'
    active = np.any(seed > 0, axis=2) | np.any(walk > 0, axis=2)
    rows = {'pixels': int(active.size), 'work_pixels': int(active.sum()),
            'work_pixel_fraction': float(active.mean()), 'counters': {}}
    for name, raw in zip(('seed_triangles', 'seed_nodes', 'projections',
                          'locate_hops', 'height_samples', 'filter_taps'),
                         [seed[:,:,i] for i in range(3)] + [walk[:,:,i] for i in range(3)]):
        lower, upper = bounds(raw)
        assert not np.any(lower > upper), 'empty decoded integer interval'
        assert not np.any(raw == 255), 'saturated log bins need unbounded upper estimates'
        population = raw > 0
        rows['counters'][name] = {
            'pixels': int(population.sum()),
            'total_range': [int(lower.sum()), int(upper.sum())],
            'active_mean_range': [float(lower[active].mean()), float(upper[active].mean())] if active.any() else [0, 0],
            'p95_active_range': [float(np.percentile(lower[active],95)), float(np.percentile(upper[active],95))] if active.any() else [0, 0],
            'max_range': [int(lower.max()), int(upper.max())],
        }
    # Joint counters distinguish a search that never starts the connected
    # walk from expensive reconstruction after a seed has been located.
    rows['seed_tests_without_locate_pixels'] = int(np.count_nonzero((seed[:,:,0]>0) & (walk[:,:,0]==0)))
    rows['seed_tests_without_height_sample_pixels'] = int(np.count_nonzero((seed[:,:,0]>0) & (walk[:,:,1]==0)))
    rows['views_aligned_projection_vs_locate'] = bool(np.all((walk[:,:,0]==0) | (seed[:,:,2]>0)))
    report['views'][view] = rows
out = root / 'work-census.json'
out.write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
