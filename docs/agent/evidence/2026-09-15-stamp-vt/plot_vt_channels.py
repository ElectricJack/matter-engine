"""Plot decoded native VT page readback; these figures are not editor screenshots."""
from pathlib import Path
import argparse
import struct
import zipfile
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

p = argparse.ArgumentParser()
p.add_argument('raw', type=Path)
p.add_argument('--out', type=Path, default=Path(__file__).parent)
args = p.parse_args()
args.out.mkdir(parents=True, exist_ok=True)
data = [np.fromfile(args.raw / f'clay-projected-{seed}.vt.bin', dtype='<f4').reshape(96, 256, 10)
        for seed in range(8)]
assert all(np.isfinite(d).all() for d in data)

def srgb(rgb):
    rgb = np.clip(rgb, 0, 1)
    return np.where(rgb <= .0031308, 12.92 * rgb, 1.055 * rgb ** (1 / 2.4) - .055)

extent = [-128, 128, -48, 48]
fig, axes = plt.subplots(4, 3, figsize=(14, 8), layout='constrained')
for seed, row in enumerate(axes):
    d = data[seed]
    channels = [srgb(d[..., :3]), d[..., 6:9] * .5 + .5, d[..., 9] * 1000]
    styles = [{}, {}, dict(cmap='viridis', vmin=-8, vmax=0)]
    for col, (ax, channel, style) in enumerate(zip(row, channels, styles)):
        im = ax.imshow(channel, extent=extent, origin='lower', interpolation='nearest', **style)
        # Atlas page boundaries converted back to physical source coordinates.
        for boundary in [-4, 124]:
            ax.axvline(boundary, color='white', alpha=.7, lw=.6, ls='--')
        ax.set_xticks([])
        ax.set_yticks([])
        if col == 0:
            ax.set_ylabel(f'Variant {seed}')
        if seed == 0:
            ax.set_title(['Unlit color (BC7)', 'Surface normal (BC5)', 'Relief relative to receiver (mm)'][col])
fig.colorbar(im, ax=axes[:, 2], shrink=.65, label='mm; base mortar = −5 mm')
fig.suptitle('Real JS-authored bricks after VT composition and compression\n'
             'Native GPU readback, 1 mm/texel; dashed lines mark page boundaries. Channel plots, not an editor view.', fontsize=13)
fig.savefig(args.out / 'vt-clay-channels.png', dpi=150)
plt.close(fig)

baseline = args.out / 'binding-v4-clay-pages.zip'
if baseline.exists():
    seed = 6
    with zipfile.ZipFile(baseline) as archive:
        before = np.frombuffer(archive.read(f'clay-projected-{seed}.vt.bin'), dtype='<f4').reshape(96, 256, 10)
    raw = (args.raw / f'clay-projected-{seed}.fst').read_bytes()
    levels = struct.unpack_from('<I', raw, 48)[0]
    source = np.frombuffer(raw, dtype='<f4', offset=136 + 16 * levels)[:96*256*16].reshape(96, 256, 16)
    coverage = np.clip(source[..., 3], 0, 1)
    scale = np.divide(coverage, source[..., 3], out=np.zeros_like(coverage), where=source[..., 3] > 0)
    expected = source[..., :3] * scale[..., None] + .2 * (1 - coverage[..., None])
    fig, axes = plt.subplots(2, 3, figsize=(12, 6), layout='constrained')
    for col, (label, rgb) in enumerate(zip(['Uncompressed source + mortar reference', 'Previous BC7 endpoint fit', 'Signed-correlation BC7 fit'],
                                          [expected, before[..., :3], data[seed][..., :3]])):
        axes[0, col].imshow(srgb(rgb), origin='lower', interpolation='nearest')
        axes[1, col].imshow(srgb(rgb[76:96, :32]), origin='lower', interpolation='nearest')
        axes[0, col].set_title(label)
        for row in range(2):
            axes[row, col].set_xticks([])
            axes[row, col].set_yticks([])
    axes[1, 0].set_ylabel('Upper-left boundary, enlarged')
    fig.suptitle('Brick–mortar boundary: identical baked source, improved VT compression\n'
                 'Native page readback versus its source reference; unlit channel plots.', fontsize=13)
    fig.savefig(args.out / 'vt-compression-comparison.png', dpi=150)
    plt.close(fig)

fig, axes = plt.subplots(4, 2, figsize=(12, 9), layout='constrained')
for seed, ax in enumerate(axes.flat):
    ax.imshow(srgb(data[seed][..., :3]), extent=extent, origin='lower', interpolation='nearest')
    ax.set_title(f'Variant {seed}')
    ax.axis('off')
fig.suptitle('Eight GPU-baked clay variants through the actual compressed VT page path\n'
             'Unlit color readback converted to sRGB; gray is the composed mortar base.', fontsize=13)
fig.savefig(args.out / 'vt-clay-variants.png', dpi=150)
plt.close(fig)
