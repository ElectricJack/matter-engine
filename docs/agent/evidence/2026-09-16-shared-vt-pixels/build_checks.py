"""Build native VT checks sequentially and bind evidence to exact source hashes."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import time

repo = Path(__file__).resolve().parents[4]
out = Path(__file__).resolve().parent
prefix = sys.argv[1]
assert re.fullmatch(r'[a-z0-9-]+', prefix)
manifest_path = out / f'{prefix}-build-manifest.json'
assert not manifest_path.exists(), 'Use a new evidence prefix for another build'


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


paths = set(json.loads((out / 'pixels-v2-sources.json').read_text()))
paths.add('MatterEngine3/src/render/vt_canonical_page.h')
paths.add('MatterEngine3/src/render/vt_seed_bvh.h')
paths.add('MatterEngine3/tests/vt_seed_bvh_tests.h')
paths.add('MatterEngine3/shaders_vk/vt_seed_walk.glsl')
paths.update([
    'MatterEngine3/shaders_vk/vt_material_domain.glsl',
    'MatterEngine3/shaders_vk/vt_material_domain_probe.comp',
    'MatterEngine3/tests/vt_material_domain_tests.h',
    'MatterEngine3/src/render/vt_periodic_material.h',
    'MatterEngine3/tests/vt_module_residency_tests.h',
    'MatterEngine3/tests/vt_pom_work_tests.h',
    'MatterEngine3/tests/vt_sector_seam_tests.h',
    'MatterEngine3/src/render/vt_surface_boundary.h',
    'MatterEngine3/src/render/vt_surface_connections.h',
    'MatterEngine3/tests/vt_surface_boundary_tests.h',
    'MatterEngine3/tests/vt_surface_connection_tests.h',
    'MatterEngine3/src/render/chart_static_surface.h',
    'MatterEngine3/src/render/vt_surface_topology.h',
    'MatterEngine3/tests/static_surface_vt_tests.cpp',
    'cmake/MatterViewer.cmake',
    'MatterEngine3/tests/vt_feedback_pair_tests.h',
    'MatterEngine3/src/render/vt_receiver_material.h',
    'MatterEngine3/src/render/vt_occlusion_pages.h',
    'MatterEngine3/src/render/vt_material_read.h',
    'MatterEngine3/tests/vt_material_read_tests.h',
    'MatterEngine3/tests/vt_receiver_material_tests.h',
    'MatterEngine3/src/render/vt_feedback_format.h',
    'MatterEngine3/src/render/vk_sparse_voxel.cpp',
    'MatterEngine3/src/render/vk_sparse_voxel.h',
    'MatterEngine3/tests/world_definition_tests.cpp',
    'MatterEngine3/tests/surface_field_tests.cpp',
    'MatterEngine3/tests/vt_cellular_fixture.h',
    'MatterEngine3/tests/chart_atlas_tests.cpp',
    'MatterEngine3/include/matter/world_definition.h',
    'MatterEngine3/src/script/world_definition_loader.cpp',
    'projects/world_demo/scenes/streaming/StreamMountain/StreamMountain.js',
    'projects/world_demo/scenes/streaming/StreamMountain/objects/WorldSector.js',
    'projects/world_demo/scenes/streaming/StreamMountain/props.json',
])
paths.update(str(p.relative_to(repo)) for p in (repo / 'projects/world_demo/shared-lib').glob('*.js'))
paths.update(str(p.relative_to(repo)) for p in
             (repo / 'projects/world_demo/scenes/texturing/terrain/SurfaceContactProof').rglob('*') if p.is_file())
paths.update(str(p.relative_to(repo)) for p in
             (repo / 'projects/world_demo/scenes/texturing/terrain/RockFormationProof').rglob('*') if p.is_file())
paths.update(str(p.relative_to(repo)) for p in
             (repo / 'projects/world_demo/scenes/texturing/terrain/RockScaleProof').rglob('*') if p.is_file())
paths.update(['projects/world_demo/objects/terrain/MountainRock.js',
              'projects/world_demo/tests/mountain_rocks_tests.mjs'])
paths.add('MatterEngine3/tests/face_material_bake_tests.cpp')
paths.add('MatterEngine3/src/render/vt_world_receivers.h')
paths.add('MatterEngine3/tests/vt_queue_tests.h')
paths.update([
    'MatterEngine3/src/asset_export.cpp',
    'third_party/raylib/src/external/stb_image_write.h',
    'MatterEngine3/src/asset_export.h',
    'MatterEngine3/include/matter/asset_export.h',
    'MatterEngine3/tests/asset_export_tests.cpp',
    'MatterEngine3/src/render/vt_export.cpp',
    'MatterEngine3/src/render/vt_export_mesh.cpp',
    'MatterEngine3/src/render/vt_export.h',
    'MatterEngine3/tests/vt_export_tests.h',
    'projects/world_demo/objects/texturing/bricks/ClayBrickWallSurface.js',
    'projects/world_demo/scenes/texturing/bricks/PeriodicBrickWallProof/PeriodicBrickWallProof.js',
    'projects/world_demo/tests/brick_surface_module_tests.mjs',
])
sources = {p: sha(repo / p) for p in sorted(paths)}
(out / f'{prefix}-sources.json').write_text(json.dumps(sources, indent=2) + '\n')
manifest = {'sources': sources, 'source_changes': [], 'binaries': {}, 'builds': []}
targets = sys.argv[2:] or ['chart_atlas_tests', 'vt_residency_tests', 'vt_compositor_tests', 'vulkan_smoke_tests', 'matter_editor']
for target in targets:
    assert re.fullmatch(r'[a-z0-9_]+', target)
    log = out / f'{prefix}-build-{target}.log'
    command = ('$env:CMAKE_BUILD_PARALLEL_LEVEL="1"; '
               f'& "./tools/build-windows.ps1" -Config RelWithDebInfo -Target {target}; exit $LASTEXITCODE')
    start = time.monotonic()
    with log.open('w') as stream:
        result = subprocess.run(['/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe',
                                 '-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', command],
                                cwd=repo, stdout=stream, stderr=subprocess.STDOUT)
    row = {'target': target, 'config': 'RelWithDebInfo', 'command': command,
           'exit': result.returncode, 'seconds': time.monotonic() - start,
           'log': str(log.relative_to(repo)), 'sha256': sha(log)}
    manifest['builds'].append(row)
    binary = repo / ('MatterEditor/build/windows-msvc/editor.exe' if target == 'matter_editor' else
                     f'MatterEditor/build/cmake/windows-msvc/relwithdebinfo/{target}.exe')
    if not result.returncode:
        manifest['binaries'][str(binary.relative_to(repo))] = sha(binary)
    manifest['source_changes'] = [p for p, h in sources.items() if sha(repo / p) != h]
    manifest_path.write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps(row), flush=True)
    if result.returncode or manifest['source_changes']:
        sys.exit(1)
