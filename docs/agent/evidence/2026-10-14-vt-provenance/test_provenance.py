"""Read-only historical source and CPU scheduling diagnostics; no GPU verdict.

Run from any checkout with both historical objects:
    aq test -n 0 docs/agent/evidence/2026-10-14-vt-provenance/test_provenance.py -q
The JSON report is written beside this file. No branch is checked out or reset.
"""
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[4]
OUT = Path(__file__).resolve().parent
BASE = '58289c187ef2565c7d0532fe28a8cfb903023e44'
MOVE = '90a19c6698a03623da9a3e3c5235ad272531d462'


def original(ref, path):
    return subprocess.check_output(['git', 'show', f'{ref}:{path}'], cwd=ROOT)


def test_historical_producer_and_vt_sources():
    report = {'baseline': BASE, 'move': MOVE, 'sha256': {}}
    host = 'MatterEngine3/src/render/vk_scene_renderer.cpp'
    source = original(MOVE, host)
    for name in ('record_gbuffer', 'create_raster_pipelines', 'impostor_atlas',
                 'adopt_part_impostors'):
        fragment = f'libs/MatterGraphics/src/native/{name}.inl'
        include = f'#include "../../../{fragment}"\n'.encode()
        assert source.count(include) == 1, fragment
        source = source.replace(include, original(MOVE, fragment))
    assert source == original(BASE, host)
    report['expanded_renderer_equal'] = True
    for name in ('raster.vert', 'gbuffer.frag', 'material_common.glsl',
                 'impostor_common.glsl'):
        assert original(MOVE, f'libs/MatterGraphics/shaders/{name}') == original(
            BASE, f'MatterEngine3/shaders_vk/{name}')
    for name in ('impostor_mips.h', 'raster_mesh.cpp', 'raster_mesh.h'):
        moved = original(MOVE, f'libs/MatterGraphics/src/native/{name}').replace(
            b'../../../../MatterEngine3/src/render/indexed_part_geometry.h',
            b'indexed_part_geometry.h')
        assert moved == original(BASE, f'MatterEngine3/src/render/{name}')
    for path in ('MatterEngine3/src/render/vt_work_budget.h',
                 'MatterEngine3/src/render/vt_types.h',
                 'MatterEngine3/src/render/vt_residency.cpp',
                 'MatterEngine3/src/render/vt_compositor.cpp',
                 'MatterEngine3/src/render/vt_compositor.h',
                 'MatterEngine3/include/matter/vt_budgets.h',
                 'MatterEngine3/tests/vulkan_smoke_tests.cpp',
                 'MatterEngine3/src/render/chart_atlas.h',
                 'MatterEngine3/src/render/vk_scene_renderer.h'):
        before = original(BASE, path)
        assert original(MOVE, path) == before, path
        report['sha256'][path] = hashlib.sha256(before).hexdigest()
    assert 'float fill_budget_ms = 4.0f;' in original(
        BASE, 'MatterEngine3/include/matter/vt_budgets.h').decode()
    assert 'supports_incremental_fill() const override { return true; }' in original(
        BASE, 'MatterEngine3/src/render/vt_compositor.h').decode()
    (OUT / 'source-comparison.json').write_text(json.dumps(report, indent=2) + '\n')


PROBE = r'''
#include "vt_work_budget.h"
#include <algorithm>
#include <cstdio>
int main() {
    vt::VtGpuWorkBudget budget;
    unsigned tiles7 = 0, tiles6 = 0;
    for (int i = 0; i < 7; ++i) {
        const auto tiles = budget.tiles();
        if (!budget.enabled() || tiles > 2) return 1;
        tiles7 += tiles;
        if (i < 6) tiles6 += tiles;
        // Optimistically cheap retired timing can never exceed the ceiling.
        budget.observe(0.01f, tiles);
    }
    const unsigned min_slices = (vt::kVtPageTiles + 1) / 2;
    std::printf("page_tiles=%u max_tiles_in_7_frames=%u max_tiles_in_6_frames=%u "
                "min_slice_frames=%u\n", vt::kVtPageTiles, tiles7, tiles6, min_slices);
    return tiles7 < vt::kVtPageTiles && tiles6 < vt::kVtPageTiles ? 0 : 2;
}
'''


def test_compiled_historical_budget_bound():
    results = []
    for ref in (BASE, MOVE):
        with tempfile.TemporaryDirectory(prefix='matter-vt-provenance-') as scratch:
            scratch = Path(scratch)
            for name in ('vt_work_budget.h', 'vt_types.h', 'chart_atlas.h'):
                (scratch / name).write_bytes(original(
                    ref, f'MatterEngine3/src/render/{name}'))
            (scratch / 'probe.cpp').write_text(PROBE)
            command = ['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                       '-I' + str(ROOT / 'third_party/Vulkan-Headers/include'),
                       str(scratch / 'probe.cpp'), '-o', str(scratch / 'probe')]
            subprocess.run(command, check=True, capture_output=True, text=True)
            run = subprocess.run([str(scratch / 'probe')], check=True,
                                 capture_output=True, text=True)
            results.append({'ref': ref, 'compiler': 'g++ -std=c++17 -Wall -Wextra -Werror',
                            'output': run.stdout.strip(), 'exit': run.returncode})
    assert results[0]['output'] == results[1]['output']
    (OUT / 'cpu-bound.json').write_text(json.dumps({
        'scope': 'CPU scheduling upper bound only; no native VT or VT-RT execution',
        'shared_dependency': 'current vendored Vulkan type declarations; no GPU calls',
        'results': results}, indent=2) + '\n')


def test_existing_vt_residency_suite():
    result = subprocess.run(
        ['make', '-C', 'MatterEngine3/tests', '-j2', 'run-vt-residency'],
        cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=180)
    (OUT / 'vt-residency.log').write_text(result.stdout)
    assert result.returncode == 0, result.stdout


def test_historical_spirv_bytes():
    import os
    compiler = Path(os.environ.get('MATTER_PROVENANCE_GLSLC',
                    '/tmp/graphics-e34-toolchain/root/usr/bin/glslc'))
    assert compiler.is_file(), 'Set MATTER_PROVENANCE_GLSLC to an installed glslc'
    env = dict(os.environ)
    # The recorded Linux SDK staging directory keeps system packages untouched.
    staged_lib = compiler.parent.parent / 'lib/x86_64-linux-gnu'
    env['LD_LIBRARY_PATH'] = str(staged_lib) + ':' + env.get('LD_LIBRARY_PATH', '')
    outputs = {}
    for ref in (BASE, MOVE):
        with tempfile.TemporaryDirectory(prefix='matter-vt-shaders-') as scratch:
            scratch = Path(scratch)
            paths = subprocess.check_output([
                'git', 'ls-tree', '-r', '--name-only', ref, '--',
                'MatterEngine3/shaders_vk', 'libs/MatterGraphics/shaders'], cwd=ROOT,
                text=True).splitlines()
            for path in paths:
                dest = scratch / path
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_bytes(original(ref, path))
            for name, define in [('raster.vert', None), ('gbuffer.frag', None),
                                 ('raster_skin.vert', 'MATTER_SKINNED_VERTEX_INPUT'),
                                 ('raster_water.vert', 'MATTER_WATER_ANIMATION_VERTEX_INPUT')]:
                source_name = 'raster.vert' if define else name
                directory = 'MatterEngine3/shaders_vk' if ref == BASE else 'libs/MatterGraphics/shaders'
                output = scratch / (name + '.spv')
                command = [str(compiler), '--target-env=vulkan1.3', '-O',
                           '-I' + str(scratch / 'MatterEngine3/shaders_vk')]
                if define:
                    command.append('-D' + define)
                command += [str(scratch / directory / source_name), '-o', str(output)]
                result = subprocess.run(command, env=env, text=True, capture_output=True, timeout=300)
                assert result.returncode == 0, result.stdout + result.stderr
                data = output.read_bytes()
                key = (ref, name)
                outputs[key] = {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
    for name in ('raster.vert', 'gbuffer.frag', 'raster_skin.vert', 'raster_water.vert'):
        assert outputs[(BASE, name)] == outputs[(MOVE, name)], name
    (OUT / 'spirv-comparison.json').write_text(json.dumps({
        'compiler': str(compiler),
        'compiler_version': subprocess.check_output([str(compiler), '--version'], env=env, text=True),
        'flags': '--target-env=vulkan1.3 -O -I<snapshot>/MatterEngine3/shaders_vk',
        'scope': 'Historical baseline/move equality with one Linux compiler; no GPU execution or Windows parity',
        'results': [{'ref': ref, 'shader': name, **value}
                    for (ref, name), value in outputs.items()]}, indent=2) + '\n')
