"""Plot decoded native GPU channels; this is a material proof, not a wall render."""
import hashlib
import json
from pathlib import Path
import re
import sys

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

out = Path(__file__).resolve().parent
prefix = sys.argv[1]
assert re.fullmatch(r'[a-z0-9-]+', prefix)
record = json.loads((out / f'{prefix}-clay.json').read_text())
assert record['passed']
raw = Path('/mnt/d/tmp/matter-vt/20260916-periodic-producer') / prefix
for path, digest in record['outputs'].items():
    assert hashlib.sha256(Path(path).read_bytes()).hexdigest() == digest
meta = json.loads((raw / 'brick-module.json').read_text())
pixels = np.fromfile(raw / 'brick-module.bin', dtype='<f4').reshape(meta['height'], meta['width'], 10)
linear = np.clip(pixels[:, :, :3], 0, 1)
albedo = np.where(linear <= .0031308, 12.92 * linear, 1.055 * linear ** (1 / 2.4) - .055)
normal = np.clip(pixels[:, :, 6:9] * .5 + .5, 0, 1)
height = pixels[:, :, 9] * 1000
period = meta['period_m']
fig, axes = plt.subplots(4, 1, figsize=(14, 12), constrained_layout=True)
fig.suptitle('Native periodic brick material — decoded GPU output\n'
             f"{meta['width']} × {meta['height']} texels, {period[0]} × {period[1]} m; no wall/POM rendering", fontsize=15)
views = [(albedo, 'Base color (linear → sRGB display)', 1, None),
         (height, 'Height relative to the outer brick datum (mm)', 1, 'gray'),
         (normal, 'Module-frame normal', 1, None),
         (np.tile(albedo, (2, 2, 1)), 'Two repeats in each direction (color only)', 2, None)]
for axis, (data, title, repeats, cmap) in zip(axes, views):
    kwargs = {'cmap': cmap} if cmap else {}
    handle = axis.imshow(data, origin='lower', interpolation='nearest',
                         extent=(0, repeats * period[0], 0, repeats * period[1]), **kwargs)
    axis.set_title(title, loc='left')
    axis.set_xlabel('metres')
    axis.set_ylabel('metres')
    if cmap:
        fig.colorbar(handle, ax=axis, fraction=.016, pad=.01, label='mm')
image = out / f'{prefix}-channels.png'
assert not image.exists()
fig.savefig(image, dpi=120)
plt.close(fig)
print(image)
