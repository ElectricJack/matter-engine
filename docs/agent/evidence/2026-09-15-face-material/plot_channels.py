"""Plot native GPU data, not an editor screenshot or a simulated beauty render."""
from pathlib import Path
import argparse
import json
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

parser = argparse.ArgumentParser()
parser.add_argument('raw', type=Path)
parser.add_argument('--out', type=Path, default=Path(__file__).parent)
args = parser.parse_args()
args.out.mkdir(parents=True, exist_ok=True)
material_dtype = np.dtype([('rgb', '<f4', 3), ('orm', '<f4', 3), ('n', '<f4', 3), ('detail', '<f4'), ('coverage', '<u4')])
geometry_dtype = np.dtype([('height', '<f4'), ('n', '<f4', 3), ('coverage', '<u4')])

def srgb(rgb):
    rgb = np.clip(rgb, 0, 1)
    return np.where(rgb <= .0031308, rgb * 12.92, 1.055 * rgb ** (1 / 2.4) - .055)

def face(seed, side):
    name = args.raw / f'clay-{seed}-{side}'
    meta = json.loads(name.with_suffix('.json').read_text())
    shape = meta['height'], meta['width']
    geometry = np.fromfile(name.with_suffix('.bin'), dtype=geometry_dtype).reshape(shape)
    material = np.fromfile(args.raw / f'clay-material-{seed}-{side}.bin', dtype=material_dtype).reshape(shape)
    assert np.array_equal(geometry['coverage'], material['coverage'])
    extent = [meta[key] * 1000 for key in ['u_min_m', 'u_max_m', 'v_min_m', 'v_max_m']]
    return meta, geometry, material, extent

names = ['Front', 'Back', 'Right end', 'Left end', 'Top', 'Bottom']
rows = ['Unlit clay color', 'Geometry depth (mm)', 'Roughness', 'Microheight (mm)', 'Material normal (UVN)']
fig, axes = plt.subplots(5, 6, figsize=(16, 10), layout='constrained')
for side in range(6):
    meta, geo, mat, extent = face(0, side)
    mask = mat['coverage'] == 0
    alpha = (~mask).astype(float)
    color = np.concatenate([srgb(mat['rgb']), alpha[..., None]], axis=-1)
    normal = np.concatenate([mat['n'] * .5 + .5, alpha[..., None]], axis=-1)
    channels = [color, np.ma.array((geo['height'] - meta['nominal_face_height_m']) * 1000, mask=mask),
                np.ma.array(mat['orm'][..., 1], mask=mask), np.ma.array(mat['detail'] * 1000, mask=mask), normal]
    styles = [{}, dict(cmap='viridis', vmin=-8, vmax=0), dict(cmap='gray', vmin=0, vmax=1),
              dict(cmap='magma', vmin=-.17, vmax=0), {}]
    for row in range(5):
        ax = axes[row, side]
        im = ax.imshow(channels[row], origin='lower', extent=extent, interpolation='nearest', **styles[row])
        ax.set_xlim(-128, 128)
        ax.set_ylim(-70, 70)
        ax.set_xticks([])
        ax.set_yticks([])
        ax.set_facecolor('#e7e7e7')
        if row == 0:
            ax.set_title(names[side])
        if side == 0:
            ax.set_ylabel(rows[row])
        if side == 5 and 0 < row < 4:
            fig.colorbar(im, ax=axes[row, :], shrink=.62, pad=.01)
fig.suptitle('Actual GPU brick-face bake: variant 0, all six orientations\n1 mm source grid; equal physical display scale; color converted from linear RGB to sRGB', fontsize=14)
fig.savefig(args.out / 'gpu-face-channels.png', dpi=130)
plt.close(fig)

fig, axes = plt.subplots(2, 4, figsize=(14, 5), layout='constrained')
for seed, ax in enumerate(axes.flat):
    _, _, mat, extent = face(seed, 0)
    rgba = np.concatenate([srgb(mat['rgb']), mat['coverage'][..., None]], axis=-1)
    ax.imshow(rgba, origin='lower', extent=extent, interpolation='nearest')
    ax.set_title(f'Variant {seed}')
    ax.axis('off')
fig.suptitle('Actual GPU front-face albedo: eight independent geometry/material seeds\nUnlit channel data; does not show the relief under scene lighting', fontsize=13)
fig.savefig(args.out / 'gpu-clay-variants.png', dpi=140)
plt.close(fig)

meta = json.loads((args.raw / 'clay-filtered-0-0.json').read_text())
data = np.fromfile(args.raw / 'clay-filtered-0-0.bin', dtype='<f4').reshape(-1, 16)
n = meta['levels'] * len(meta['fractions']) * meta['grid_height'] * meta['grid_width']
grid = data[:n].reshape(meta['levels'], len(meta['fractions']), meta['grid_height'], meta['grid_width'], 16)
fig, axes = plt.subplots(2, 5, figsize=(14, 5), layout='constrained')
for column, level in enumerate([0, 2, 4, 6, 8]):
    p = grid[level, 0]
    coverage = np.clip(p[..., 3], 0, 1)
    # Composite in linear space on neutral grey solely to make finite alpha visible.
    rgb = p[..., :3] * coverage[..., None] + .35 * (1 - coverage[..., None])
    axes[0, column].imshow(srgb(rgb), origin='lower', interpolation='nearest')
    axes[1, column].imshow(coverage, origin='lower', interpolation='nearest', cmap='gray', vmin=0, vmax=1)
    axes[0, column].set_title(f'{2**level:g} mm footprint')
    for row in range(2):
        axes[row, column].set_xticks([])
        axes[row, column].set_yticks([])
axes[0, 0].set_ylabel('Filtered color over grey')
axes[1, 0].set_ylabel('Coverage')
fig.suptitle('Native GPU finite-source sampling across distances\nSame source and query positions; no UV repeat; subpixel coverage follows the footprint area', fontsize=13)
fig.savefig(args.out / 'gpu-finite-filter.png', dpi=140)
plt.close(fig)
print('Wrote GPU channel, variant and finite-filter figures.')
