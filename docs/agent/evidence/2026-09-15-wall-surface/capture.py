"""Sequential native material views; run from any WSL cwd. Never edits sources."""
from pathlib import Path
import argparse, hashlib, json, re, shutil, subprocess, time
from PIL import Image

parser = argparse.ArgumentParser()
parser.add_argument('proof_kind', choices=['brick', 'terrain'])
parser.add_argument('label')
parser.add_argument('--build-manifest', required=True)
parser.add_argument('--world', default='ClayBrickWallSurfaceProof', choices=['ClayBrickWallSurfaceProof', 'SharedBrickWallProof', 'PeriodicBrickWallProof'])
parser.add_argument('--tpm', type=int, default=256)
parser.add_argument('--enrich-pages', type=int, choices=range(17), default=0)
parser.add_argument('--exposure', type=float, default=-1)
parser.add_argument('--compare-pom', action='store_true')
parser.add_argument('--review-lighting', action='store_true')
parser.add_argument('--debug-views', action='store_true')
parser.add_argument('--all-faces', action='store_true')
parser.add_argument('--pom-diagnostics', action='store_true')
args = parser.parse_args()
assert re.fullmatch(r'[a-z0-9-]+', args.label)
repo = Path(__file__).resolve().parents[4]
out = repo / 'docs/agent/evidence/2026-09-15-wall-surface'
out.mkdir(parents=True, exist_ok=True)
external = Path('/mnt/d/tmp/matter-vt/20260915-wall-surface')
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
for p in ['projects/world_demo/objects/texturing/bricks/ClayBrickWallSurface.js','projects/world_demo/objects/texturing/bricks/ClayBrickSurface.js','projects/world_demo/shared-lib/clay_brick_surface.js','projects/world_demo/objects/texturing/bricks/ClayBrickWall.js','projects/world_demo/shared-lib/brick_wall_geometry.js','projects/world_demo/shared-lib/brick_wall_layout.js','projects/world_demo/shared-lib/castle_surface_shells.js','projects/world_demo/objects/texturing/bricks/ClayBrickSource.js','projects/world_demo/shared-lib/clay_brick_source.js','projects/world_demo/shared-lib/clay_brick_material.js']:
    source[p] = sha(repo / p)
for directory in ['scenes', 'objects', 'shared-lib']:
    for extension in ['*.js', '*.json']:
        for p in (repo / 'projects/world_demo' / directory).rglob(extension):
            source[str(p.relative_to(repo))] = sha(p)
views = {'group':[1.75,1.15,2.9,.25,.23,.08], 'close':[.94,.65,1.2,.48,.25,.15], 'grazing':[1.04,.8,.55,.57,.25,.12], 'rt':[.94,.65,1.2,.48,.25,.15]}
if world == 'SharedBrickWallProof':
    views = {'group':[7,3.6,10.5,2.5,.9,.12],
             'close-left':[-1.2,1,1.5,-1.4,.8,.12],
             'close':[1.3,1,1.5,1.1,.8,.12],
             'close-right':[4.8,1,1.5,4.6,.8,.12],
             'grazing':[2.8,1,.65,1.1,.8,.12], 'rt':[1.3,1,1.5,1.1,.8,.12]}
if world == 'PeriodicBrickWallProof':
    views = {'group':[11,6,17,5.1,.75,.1225],
             'close-left':[-2,.22,.95,-2,.18,.245],
             'close':[1,.40,.90,1,.37,.245],
             'close-right':[6,.8,1.65,6,.7,.245],
             'grazing':[3.5,.48,.55,1,.37,.245],
             'rt':[1,.40,.90,1,.37,.245]}
if args.all_faces:
    views.update({'back':[-.7,.6,-1.3,.3,.22,.08], 'left':[-1.7,.55,.45,.3,.22,.12],
                  'top':[.35,1.8,.13,.35,.22,.12], 'bottom':[.35,-1.2,.2,.35,.22,.12]})
debug_modes = {'albedo': 4, 'normals': 1, 'wireframe': 6}
if args.debug_views:
    views.update({name: views['close'] for name in debug_modes})
pom_modes = {'pom-status': 7, 'pom-charts': 8}
if args.pom_diagnostics:
    views.update({name: views['close'] for name in pom_modes})
    debug_modes.update({name: 4 for name in pom_modes})
lines = ['wait_event bake.finished 180', 'wait_idle 2 180',
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
    lines += ['render_path raster', 'cam ' + ' '.join(map(str, cam))]
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
                  f'shot D:/tmp/matter-vt/20260915-wall-surface/{name}']
lines += ['set render.pom.horizon_debug 0', 'set viewer.debug.debug_view_mode 0', 'quit']
commands = external / f'{args.label}-commands.txt'
commands.write_text('\n'.join(lines) + '\n')
env = {'MATTER_WORLD': world, 'MATTER_VT_PROP_TEXELS_PER_METER': str(args.tpm),
       'MATTER_VT_ENRICH_PER_FRAME': str(args.enrich_pages),
       'MATTER_VT_CHART_LOG': '1', 'MATTER_HIDE_UI': '1', 'MATTER_HIDE_WINDOW': '1',
       'MATTER_WINDOW_WIDTH': '1280', 'MATTER_WINDOW_HEIGHT': '800',
       'MATTER_CMD_FIFO': f'D:/tmp/matter-vt/20260915-wall-surface/{commands.name}',
       'MATTER_VK_VALIDATION': '1'}
ps = '; '.join(f"$env:{k}='{v}'" for k, v in env.items())
ps += '; $env:TMP="$env:LOCALAPPDATA\\Temp"; $env:TEMP=$env:TMP'
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
              r'FATAL:|bake failed|bake error|BakeError|normal-offset projection did not converge|finite-surface.*binding|Validation Error|VUID-|timeout after|dispatch failed|native_rt unavailable|too many ops|set: unknown property|set: cannot parse|set: .*forced by', line)],
          'vt_stats': re.findall(r'STATSVT[^\r\n]+', log), 'images': {}}
for name in images:
    image = external / name
    if image.exists() and Path(str(image) + '.done').exists():
        shutil.copy2(image, out / name)
        result['images'][name] = sha(image)
        with Image.open(image) as pixels:
            extrema = pixels.convert('RGB').getextrema()
            uniform_success = ('-pom-status' in name and extrema[1][0] > 128 and
                               extrema[0][1] < extrema[1][0] / 4 and
                               extrema[2][1] < extrema[1][0] / 4)
            # A close wall can fill the frame with one chart and successful
            # displacement everywhere. Shaded views still prove its presence;
            # a uniform non-green status remains an error.
            if (max(hi - lo for lo, hi in extrema) < 8 and
                    '-pom-charts' not in name and not uniform_success):
                result['errors'].append(f'{name}: nearly uniform capture; receiver may be absent')
result['passed'] = not (result['exit'] or result['source_changes'] or result['binary_changed'] or
                        result['errors'] or len(result['images']) != len(images))
(out / f'{args.label}-result.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result), flush=True)
raise SystemExit(0 if result['passed'] else 1)
