"""Check production Windows source inventories without a native configure/build.

Evaluate the actual manifest reader and inventory construction with CMake -P,
then bind that inventory to the unmodified helper's viewer census/member block.
This does not qualify target definitions, links, compilation, or native runtime.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import time


def section(text, start, end):
    return text[text.index(start):text.index(end, text.index(start))]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[3])
    parser.add_argument('--helper', type=Path)
    args = parser.parse_args()
    root = args.root.resolve()
    helper_path = args.helper or root / 'cmake/tests/viewer_graph_tests.cmake'
    helper = helper_path.read_text()
    engine = (root / 'cmake/MatterEngine.cmake').read_text()
    viewer = (root / 'cmake/MatterViewer.cmake').read_text()
    engine_inventory = section(engine, 'matter_read_manifest(', 'add_library(matter_engine_core OBJECT')
    viewer_inventory = section(viewer, 'matter_read_manifest(', 'add_library(matter_engine_viewer_objects OBJECT')
    contains = section(helper, 'function(assert_list_contains', 'endfunction()') + 'endfunction()\n'
    binding = 'get_target_property(viewer_sources matter_engine_viewer_objects SOURCES)'
    predicate = section(helper, binding, 'get_target_property(viewer_definitions')
    assert predicate.count(binding) == 1
    predicate = predicate.replace(binding, 'set(viewer_sources ${matter_engine_viewer_product_sources})')
    inputs = [
        'cmake/ManifestSources.cmake', 'cmake/MatterEngine.cmake', 'cmake/MatterViewer.cmake',
        'cmake/manifests/engine-core.sources', 'cmake/manifests/matter-surface.sources',
        'cmake/manifests/engine-viewer.sources',
    ]
    result = {
        'qualification': 'Portable production inventory and helper predicate only; not native graph/build/runtime qualification.',
        'helper_sha256': hashlib.sha256(helper_path.read_bytes()).hexdigest(),
        'inputs_sha256': {p: hashlib.sha256((root / p).read_bytes()).hexdigest() for p in inputs},
        'runs': [],
    }
    with tempfile.TemporaryDirectory(prefix='matter-viewer-census-') as scratch:
        scratch = Path(scratch)
        for mode in ('ON', 'OFF'):
            output = scratch / f'{mode}.txt'
            script = scratch / f'{mode}.cmake'
            script.write_text(
                'cmake_minimum_required(VERSION 3.25)\n'
                f'set(CMAKE_SOURCE_DIR [==[{root}]==])\n'
                'set(WIN32 TRUE)\n'
                f'set(MATTER_ENABLE_AUTOREMESHER {mode})\n'
                f'set(MATTER_EXPECT_RETOPO {mode})\n'
                f'include([==[{root / "cmake/ManifestSources.cmake"}]==])\n'
                + engine_inventory + viewer_inventory + contains
                + 'list(LENGTH matter_engine_core_sources core_count)\n'
                + 'list(LENGTH matter_engine_surface_sources surface_count)\n'
                + f'file(WRITE [==[{output}]==] "core=${{core_count}}\nsurface=${{surface_count}}\nviewer=${{matter_viewer_product_count}}\nunique=${{matter_viewer_unique_count}}\nsources=${{matter_engine_viewer_product_sources}}\n")\n'
                + predicate
                + f'file(APPEND [==[{output}]==] "c_sources=${{viewer_c_count}}\n")\n'
            )
            command = ['cmake', '-P', str(script)]
            started = time.monotonic()
            run = subprocess.run(command, text=True, capture_output=True)
            inventory = dict(line.split('=', 1) for line in output.read_text().splitlines())
            sources = inventory.pop('sources').split(';')
            inventory = {k: int(v) for k, v in inventory.items()}
            assert inventory['core'] == 170 and inventory['surface'] == 21
            assert inventory['viewer'] == inventory['unique'] == (221 if mode == 'ON' else 220)
            assert sum(p.endswith('.c') for p in sources) == 3
            assert len(sources) == len(set(sources))
            for required in ('libs/MatterGraphics/src/native/mesh_raster.cpp',
                             'libs/MatterGraphics/src/native/mesh_pipelines.cpp'):
                assert required in sources
            assert all((root / p).is_file() for p in sources)
            result['runs'].append({
                'retopology': mode, 'exit': run.returncode,
                'elapsed_seconds': round(time.monotonic() - started, 4),
                'inventory': inventory, 'sources': sources,
                'stdout': run.stdout, 'stderr': run.stderr,
            })
    print(json.dumps(result, indent=2))
    return 0 if all(r['exit'] == 0 for r in result['runs']) else 1


if __name__ == '__main__':
    raise SystemExit(main())
