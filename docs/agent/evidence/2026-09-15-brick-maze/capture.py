"""Sequential native material views; run from any WSL cwd. Never edits sources."""
from pathlib import Path
import argparse, hashlib, json, re, shutil, subprocess, time
from PIL import Image

parser = argparse.ArgumentParser()
parser.add_argument('proof_kind', choices=['brick', 'terrain'])
parser.add_argument('label')
parser.add_argument('--build-manifest', required=True)
parser.add_argument('--world', default='ClayBrickMaze', choices=['ClayBrickMaze'])
parser.add_argument('--tpm', type=int, default=256)
parser.add_argument('--frames', type=int, default=30)
parser.add_argument('--exposure', type=float, default=-1)
parser.add_argument('--compare-pom', action='store_true')
parser.add_argument('--review-lighting', action='store_true')
parser.add_argument('--debug-views', action='store_true')
parser.add_argument('--all-faces', action='store_true')
parser.add_argument('--pom-diagnostics', action='store_true')
parser.add_argument('--weathering-tags', action='store_true')
parser.add_argument('--alignment-review', action='store_true')
args = parser.parse_args()
assert re.fullmatch(r'[a-z0-9-]+', args.label)
repo = Path('/mnt/d/Shared With Desktop/AI/matter-engine-cpp')
out = repo / 'docs/agent/evidence/2026-09-15-brick-maze'
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
# Scenes/objects are grouped by domain now. Include the authored dependencies,
# not the old flat scene path (whose rglob silently returned no files).
for root in ['scenes', 'objects', 'shared-lib']:
    for extension in ['*.js', '*.json']:
        for p in (repo / 'projects/world_demo' / root).rglob(extension):
            source[str(p.relative_to(repo))] = sha(p)
views = {'overview':[27,24,30,9.3,.5,9.3],
    'corner':[5.6,1.35,4.9,4.4,.7,3.4],
    'curve':[8.8,1.3,12.3,7.25,.8,10.65],
    'tight-curve':[8,1.5,6.3,10.2,1,7.8],
    'walkthrough':[9.25,1.65,.9,9.25,1.2,10],
    'close':[7.6,.85,11.4,7.15,.65,10.9]}
if args.weathering_tags:
    views.update({'painted-court':[8.6,1.2,1.9,9.3,.72,3.3],
                  'tag-divider':[2.65,1.35,10.6,2.9,1,8.35]})
if args.all_faces:
    views.update({'back':[-.7,.6,-1.3,.3,.22,.08], 'left':[-1.7,.55,.45,.3,.22,.12],
                  'top':[.35,1.8,.13,.35,.22,.12], 'bottom':[.35,-1.2,.2,.35,.22,.12]})
debug_modes = {'albedo': 4, 'normals': 1, 'wireframe': 6}
if args.alignment_review:
    views = {'corner':views['corner'],
             'long-wall':[23.8,3.3,13.8,18.5,1.15,13.8],
             'curve':views['curve']}
    for name, camera in list(views.items()):
        for suffix, mode in [('albedo',4),('normals',1)]:
            key=name+'-'+suffix
            views[key]=camera
            debug_modes[key]=mode
if args.debug_views:
    views.update({name: views['close'] for name in debug_modes})
pom_modes = {'pom-status': 7, 'pom-charts': 8}
if args.pom_diagnostics:
    views.update({name: views['close'] for name in pom_modes})
    debug_modes.update({name: 4 for name in pom_modes})
lines = ['wait_event bake.finished 300', 'wait_idle 2 300',
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
lines += ['wait_frames 90']
for view, cam in views.items():
    lines += ['render_path raster', 'cam ' + ' '.join(map(str, cam)),
              f'set viewer.debug.debug_view_mode {debug_modes.get(view,0)}']
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
        lines += [f'wait_frames {args.frames}', f'stats {args.label}-{view}{suffix}',
                  f'shot D:/tmp/matter-vt/20260915-wall-surface/{name}']
lines += ['set render.pom.horizon_debug 0', 'set viewer.debug.debug_view_mode 0', 'quit']
commands = external / f'{args.label}-commands.txt'
commands.write_text('\n'.join(lines) + '\n')
env = {'MATTER_FRAME_LIMIT':'0', 'MATTER_DLSS_MODE':'0', 'MATTER_DISABLE_VK_RT':'1', 'MATTER_WORLD': world, 'MATTER_VT_PROP_TEXELS_PER_METER': str(args.tpm),
       'MATTER_VT_CHART_LOG': '1', 'MATTER_HIDE_UI': '1', 'MATTER_HIDE_WINDOW': '0',
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
              r'FATAL:|[b]?ake failed|[b]?ake error|BakeError|normal-offset projection did not converge|finite-surface.*binding|Validation Error|VUID-|timeout after|dispatch failed|native_rt unavailable|too many ops|set: unknown property|set: cannot parse|set: .*forced by', line)],
          'vt_stats': re.findall(r'STATSVT[^\r\n]+', log), 'images': {}}
for name in images:
    image = external / name
    if image.exists() and Path(str(image) + '.done').exists():
        shutil.copy2(image, out / name)
        result['images'][name] = sha(image)
        with Image.open(image) as pixels:
            if max(hi - lo for lo, hi in pixels.convert('RGB').getextrema()) < 8:
                result['errors'].append(f'{name}: nearly uniform capture; receiver may be absent')
result['vt_capture_ready'] = len(result['vt_stats']) == len(images) and all(
    re.search(r',queue=0(?:,|$)', sample) for sample in result['vt_stats'])
if not result['vt_capture_ready']:
    result['errors'].append('VT capture did not record an empty queue for every view')
result['passed'] = not (result['exit'] or result['source_changes'] or result['binary_changed'] or
                        result['errors'] or len(result['images']) != len(images))
(out / f'{args.label}-result.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result), flush=True)
raise SystemExit(0 if result['passed'] else 1)
