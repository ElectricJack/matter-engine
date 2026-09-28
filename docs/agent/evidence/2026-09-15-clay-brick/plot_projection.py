"""Plot native geometry projection data, not a rendered/retouched brick image."""
from pathlib import Path
import json
import argparse
import zipfile
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

root = Path(__file__).resolve().parent
parser = argparse.ArgumentParser()
parser.add_argument('--prefix', default='clay-detail-v4')
args = parser.parse_args()
record = np.dtype([('height', '<f4'), ('normal', '<f4', (3,)), ('coverage', '<u4')])
fig, axes = plt.subplots(2, 2, figsize=(12, 5.4), layout='constrained')
with zipfile.ZipFile(root / (args.prefix + '-faces.zip')) as archive:
    for side in range(2):
        name = f'clay-0-{side}'
        meta = json.loads(archive.read(name + '.json'))
        a = np.frombuffer(archive.read(name + '.bin'), dtype=record).reshape(meta['height'], meta['width'])
        covered = a['coverage'] != 0
        extent = [1000 * meta[k] for k in ['u_min_m', 'u_max_m', 'v_min_m', 'v_max_m']]
        recess = np.ma.array(1000 * (meta['nominal_face_height_m'] - a['height']), mask=~covered)
        heat = axes[0, side].imshow(recess, origin='lower', extent=extent,
            cmap='magma', vmin=0, vmax=8, interpolation='nearest')
        axes[0, side].set_title(('Front' if side == 0 else 'Back') + ': recess from nominal face')
        rgba = np.concatenate([a['normal'] * 0.5 + 0.5, covered[..., None]], axis=2)
        axes[1, side].imshow(rgba, origin='lower', extent=extent, interpolation='nearest')
        axes[1, side].set_title('Projected surface normals in face coordinates')
        for axis in axes[:, side]:
            axis.set_facecolor('#e5e5e5')
            axis.set_xlabel('U (mm)')
            axis.set_ylabel('V (mm)')
fig.colorbar(heat, ax=list(axes[0]), label='Recess (mm; clipped at 8)', shrink=0.85)
fig.suptitle('Actual GPU bake — clay variant 0, 1 mm sampling\nGray = outside finite brick coverage; no clay appearance baked yet', fontsize=13)
fig.savefig(root / (args.prefix + '-geometry.png'), dpi=150)
