"""Sequential native material views; run from any WSL cwd. Never edits sources."""
from pathlib import Path
import argparse, hashlib, json, re, shutil, subprocess, time
from PIL import Image

parser = argparse.ArgumentParser()
parser.add_argument('world', choices=['brick', 'terrain'])
parser.add_argument('label')
parser.add_argument('--build-manifest', required=True)
parser.add_argument('--world', default='ClayBrickGeometryProof', choices=['ClayBrickGeometryProof', 'ClayBrickMaterialProof'])
parser.add_argument('--tpm', type=int, default=256)
parser.add_argument('--exposure', type=float, default=-1)
parser.add_argument('--compare-pom', action='store_true')
parser.add_argument('--review-lighting', action='store_true')
parser.add_argument('--debug-views', action='store_true')
parser.add_argument('--pom-diagnostics', action='store_true')
args = parser.parse_args()
assert re.fullmatch(r'[a-z0-9-]+', args.label)
repo = Path('/mnt/d/Shared With Desktop/AI/matter-engine-cpp')
out = repo / 'docs/agent/evidence/2026-09-15-clay-brick'
out.mkdir(parents=True, exist_ok=True)
external = Path('/mnt/d/tmp/matter-vt/20260915-material-look')
external.mkdir(parents=True, exist_ok=True)
def sha(p):
    with p.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()
build = json.loads((repo / args.build_manifest).read_text())
assert all(sha(repo / p) == h for p, h in build['sources'].items()), 'Build sources changed'
binary = repo / 'MatterEditor/build/windows-msvc/editor.exe'
assert sha(binary) == build['binaries'][str(binary.relative_to(repo))], 'Editor changed'
source = dict(build['sources'])
world = args.world
for p in ['projects/world_demo/objects/texturing/bricks/ClayBrickSource.js','projects/world_demo/shared-lib/clay_brick_source.js','projects/world_demo/shared-lib/clay_brick_material.js']:
    source[p] = sha(repo / p)
for p in (repo / 'projects/world_demo/scenes' / world).rglob('*.js'):
    source[str(p.relative_to(repo))] = sha(p)
views = {'group': [0.14,.20,1.1,0,.095,0], 'close': [-.31,.115,.34,-.405,.043,.025], 'grazing': [-.14,.105,.27,-.405,.043,.025], 'rt': [-.31,.115,.34,-.405,.043,.025]}
if world == 'ClayBrickMaterialProof':
    views = {'group': [.14,.20,.7,0,.043,0], 'close': [.095,.115,.34,0,.043,.025], 'grazing': [.265,.105,.27,0,.043,.025], 'rt': [.095,.115,.34,0,.043,.025]}
debug_modes = {'albedo': 4, 'normals': 1, 'wireframe': 6}
if args.debug_views:
    views.update({name: views['close'] for name in debug_modes})
pom_modes = {'pom-status': 7, 'pom-charts': 8}
if args.pom_diagnostics:
    views.update({name: views['close'] for name in pom_modes})
    debug_modes.update({name: 4 for name in pom_modes})
lines = ['wait_event bake.finished 60', 'wait_idle 2 60',
         f'set render.lighting.exposure_ev {args.exposure}', 'render_path raster']
review_lighting = {'sun_multiplier': 1.67, 'sky_multiplier': .77,
                  'day_ambient_multiplier': .5, 'twilight_ambient_multiplier': .25,
                  'sky_irradiance_multiplier': .7, 'sunset_direct_ratio': .25,
                  'sun_tint': '1,1,1', 'sky_tint': '1,0.9,0.75'}
if args.review_lighting:
    lines += [f'set render.lighting.{key} {value}' for key, value in review_lighting.items()]
lines += ['set viewer.debug.debug_view_mode 0', 'set render.pom.horizon_debug 0',
          'set render.pom.enabled true']
pom_settings = {'relief_cap_m': .1, 'max_march_m': .3, 'steps': 32}
lines += [f'set render.pom.{key} {value}' for key, value in pom_settings.items()]
images = []
for view, cam in views.items():
    lines += ['cam ' + ' '.join(map(str, cam))]
    if view == 'rt':
        lines += ['render_path native_rt']
    if view in debug_modes:
        lines += ['render_path raster', f'set viewer.debug.debug_view_mode {debug_modes[view]}']
    if view in pom_modes:
        lines += [f'set render.pom.horizon_debug {pom_modes[view]}']
    for suffix in (['-off', ''] if args.compare_pom else ['']):
        name = f'{args.label}-{view}{suffix}.png'
        images.append(name)
        if args.compare_pom:
            lines += ['set render.pom.enabled ' + ('false' if suffix else 'true')]
        lines += ['wait_frames 180', f'stats {args.label}-{view}{suffix}',
                  f'shot D:/tmp/matter-vt/20260915-material-look/{name}']
lines += ['set render.pom.horizon_debug 0', 'set viewer.debug.debug_view_mode 0', 'quit']
commands = external / f'{args.label}-commands.txt'
commands.write_text('\n'.join(lines) + '\n')
env = {'MATTER_WORLD': world, 'MATTER_VT_PROP_TEXELS_PER_METER': str(args.tpm),
       'MATTER_VT_CHART_LOG': '1', 'MATTER_HIDE_UI': '1', 'MATTER_HIDE_WINDOW': '1',
       'MATTER_WINDOW_WIDTH': '1280', 'MATTER_WINDOW_HEIGHT': '800',
       'MATTER_CMD_FIFO': f'D:/tmp/matter-vt/20260915-material-look/{commands.name}',
       'MATTER_VK_VALIDATION': '1'}
ps = '; '.join(f"$env:{k}='{v}'" for k, v in env.items())
ps += "; Remove-Item Env:MATTER_SCREENSHOT -ErrorAction SilentlyContinue"
ps += "; Set-Location -LiteralPath 'D:/Shared With Desktop/AI/matter-engine-cpp/MatterEditor'"
ps += "; & './build/windows-msvc/editor.exe'; exit $LASTEXITCODE"
cmd = ['/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe',
       '-NoProfile', '-NonInteractive', '-Command', ps]
manifest = {'sources': source, 'binary_sha256': sha(binary), 'build_manifest': args.build_manifest,
            'environment': env, 'exposure_ev': args.exposure, 'views': views, 'timeline': lines,
            'review_lighting': review_lighting if args.review_lighting else None,
            'pom_comparison': args.compare_pom, 'pom_settings': pom_settings}
(out / f'{args.label}-source.json').write_text(json.dumps(manifest, indent=2) + '\n')
start = time.monotonic()
log_path = out / f'{args.label}.log'
with log_path.open('w') as log:
    run = subprocess.run(cmd, cwd=repo, stdout=log, stderr=subprocess.STDOUT)
log = log_path.read_text(errors='replace')
result = {'exit': run.returncode, 'seconds_including_startup_and_view_waits': time.monotonic() - start,
          'source_changes': [p for p, h in source.items() if sha(repo / p) != h],
          'binary_changed': sha(binary) != manifest['binary_sha256'],
          'errors': [line for line in log.splitlines() if re.search(
              r'Validation Error|VUID-|timeout after|dispatch failed|native_rt unavailable|too many ops|set: unknown property|set: cannot parse|set: .*forced by', line)],
          'vt_stats': re.findall(r'STATSVT[^\r\n]+', log), 'images': {}}
for name in images:
    image = external / name
    if image.exists() and Path(str(image) + '.done').exists():
        shutil.copy2(image, out / name)
        result['images'][name] = sha(image)
        with Image.open(image) as pixels:
            if max(hi - lo for lo, hi in pixels.convert('RGB').getextrema()) < 8:
                result['errors'].append(f'{name}: nearly uniform capture; receiver may be absent')
result['passed'] = not (result['exit'] or result['source_changes'] or result['binary_changed'] or
                        result['errors'] or len(result['images']) != len(images))
(out / f'{args.label}-result.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result), flush=True)
raise SystemExit(0 if result['passed'] else 1)
