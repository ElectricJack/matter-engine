#!/usr/bin/env python3
"""Generate a separate Linux CTest project for actual core/CPU editor binaries.

Root CMake remains Windows-only. Core executables come from the existing Make
targets; editor executables use their canonical CMake source closure. Missing
or unsupported executables fail registration, never become passing stubs.
Use --only for a named, explicitly partial validation graph.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess


def quote(value):
    return '[==[' + str(value) + ']==]'


def discover(repository):
    engine = (repository / 'cmake/MatterEngine.cmake').read_text()
    by_source = {}
    for directory in ['MatterEngine3/tests', 'libs/MatterSurfaceLib/tests']:
        make = (repository / directory / 'Makefile').read_text().replace('\\\n', ' ')
        variables = {}
        for key, operator, value in re.findall(
                r'^(\w+)[ \t]*([:?+]?=)[ \t]*(.*)$', make, re.M):
            if operator == '+=':
                variables[key] = variables.get(key, '') + ' ' + value
            elif operator != '?=' or key not in variables:
                variables[key] = value
        for key, value in variables.items():
            if not key.endswith('_TARGET') or not value.startswith('build/'):
                continue
            source = variables.get(key[:-7] + '_CPP',
                                   variables.get(key[:-7] + '_SOURCES', '')).split()
            if source and '$' not in source[0]:
                by_source[Path(source[0]).name] = {
                    'directory': directory, 'binary': value.strip(),
                    'build_target': value.strip()}
        # Literal standalone recipes use the same real source, flags and link
        # closure as Make; do not recreate a header-only test as a passing stub.
        for target, source in re.findall(
                r'^(build/[\w.-]+):[ \t]+([\w./-]+\.cpp)(?:[ \t]|$)', make, re.M):
            by_source[Path(source).name] = {
                'directory': directory, 'binary': target, 'build_target': target}
    for name, recipe in [('async_bake_tests', 'async-bake-tests')]:
        by_source[name + '.cpp'] = {'directory': 'MatterEngine3/tests',
                                    'binary': 'build/' + name, 'build_target': recipe}
    by_source['fixture_io_tests.cpp'] = {
        'directory': 'MatterEngine3/tests', 'binary': 'build/fixture_io_tests',
        'build_target': 'build/fixture_io_tests'}
    peer_graphs = set()
    core = {}
    unsupported = {}
    for match in re.finditer(r'matter_add_engine_cpu_test\((\w+)\s+([^)]*)\)', engine):
        name, body = match.groups()
        if name in peer_graphs:
            continue
        sources = body.split()
        if not sources or any('$' in source for source in sources):
            unsupported[name] = 'core source closure is not literal'
            continue
        source = sources[0]
        target = by_source.get(Path(source).name)
        if target:
            core[name] = dict(target, source=source, sources=sources)
        else:
            core[name] = {'source': source, 'sources': sources, 'archive': True}

    # Include literal CPU executables outside the shared helper (math,
    # residency and portability guards), using their canonical source closure.
    for name, body in re.findall(r'add_executable\((\w+)\s+([^)]*)\)', engine):
        if not re.search(r'set_tests_properties\(' + re.escape(name) +
                         r'\s+PROPERTIES\s+LABELS\s+"?cpu(?:"|\s|\))', engine):
            continue
        sources = body.split()
        if sources and all('$' not in source for source in sources):
            core[name] = {'source': sources[0], 'sources': sources, 'archive': True}

    editor_text = (repository / 'cmake/MatterEditor.cmake').read_text()
    for name, body in re.findall(r'target_compile_definitions\((\w+)\s+PRIVATE\s+([^)]*)\)', engine):
        if name in core:
            definitions = [item for item in body.split()
                           if not item.strip('"').startswith("MATTER_SCRIPT_TEST_MODULES_HEADER=")]
            if any('$' in item for item in definitions):
                unsupported[name] = 'core compile definitions are not literal'
                del core[name]
            else:
                core[name].setdefault('compile_definitions', []).extend(definitions)
    editor = {}
    for match in re.finditer(r'add_executable\((\w+)\s+([^)]*)\)', editor_text):
        name, body = match.groups()
        if not re.search(r'set_tests_properties\(' + re.escape(name) +
                         r'\s+PROPERTIES\s+LABELS\s+"editor;cpu"\)', editor_text):
            continue
        sources = body.split()
        if not sources or any('$' in source for source in sources):
            unsupported[name] = 'editor source closure is not literal'
        else:
            editor[name] = sources
    # The native suite includes the same verbatim CPU contracts. The Linux
    # executable avoids compiling Win32-only GPU diagnostic test functions,
    # while linking the actual renderer/device and CPU implementation TUs.
    viewer = (repository / 'cmake/MatterViewer.cmake').read_text()
    closure = re.search(r'set\(matter_vulkan_cpu_contract_sources\s+([^)]*)\)', viewer)
    if closure is None or any('$' in item for item in closure[1].split()):
        unsupported['vulkan_smoke_tests'] = 'Vulkan CPU production closure is not literal'
    else:
        source = 'MatterEngine3/tests/vulkan_cpu_tests.cpp'
        core['vulkan_smoke_tests'] = {
            'source': source, 'sources': [source, 'MatterEditor/src/ui_lighting_controls.cpp'] + closure[1].split(),
            'archive': True, 'vulkan_cpu': True,
            'compile_definitions': ['MATTER_HAVE_STREAMLINE=0', 'MATTER_VK_TEST_FAULT_INJECTION']}
    geometry_test = re.search(r'add_executable\(geometry_runtime_tests\s+([^)]*)\)', viewer)
    manifest = (repository / 'cmake/manifests/engine-viewer.sources').read_text()
    geometry_sources = [line.split('|')[0] for line in manifest.splitlines()
                        if line.split('|')[0].endswith('/geometry_world_runtime.cpp')]
    if geometry_test is None or len(geometry_sources) != 1:
        unsupported['geometry_runtime_tests'] = 'geometry runtime source closure is unavailable'
    else:
        test_sources = geometry_test[1].split()
        if len(test_sources) != 1 or '$' in test_sources[0]:
            unsupported['geometry_runtime_tests'] = 'geometry runtime test source is not literal'
        else:
            core['geometry_runtime_tests'] = {
                'source': test_sources[0], 'sources': test_sources + geometry_sources,
                'archive': True, 'vulkan_cpu': True,
                'compile_definitions': ['MATTER_GEOMETRY_RUNTIME_CPU_TEST']}
    environment_loop = re.search(r'foreach\(suite\s+([^)]*)\)(.*?)endforeach\(\)', engine, re.S)
    if environment_loop and 'matter_qualify_environment_test' in environment_loop[2]:
        for suite in environment_loop[1].split():
            if not re.fullmatch(r'\w+', suite):
                raise ValueError('environment suite name is not literal: '+suite)
            name, source = suite+'_tests', 'MatterEngine3/tests/'+suite+'_tests.cpp'
            core[name] = {'source': source, 'sources': [source], 'archive': True,
                          'environment_qualification': True}
    return core, editor, unsupported, sorted(peer_graphs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repository', type=Path,
                        default=Path(__file__).resolve().parents[2])
    parser.add_argument('--test-dir', type=Path, required=True)
    parser.add_argument('--only', nargs='+', help='explicit partial graph')
    parser.add_argument('--build-core', action='store_true')
    parser.add_argument('--glslc', default='glslc', help='actual Linux shader compiler for the Make archive')
    parser.add_argument('--vulkan-layer-dir', type=Path,
                        help='actual Vulkan validation-layer manifests for the compute GPU test')
    parser.add_argument('--vulkan-layer-library-dir', type=Path,
                        help='actual validation-layer shared-library directory when outside the system loader path')
    parser.add_argument('--archive-core', action='store_true',
                        help='link every common core test against the canonical Make headless archive')
    args = parser.parse_args()
    repository = args.repository.resolve()
    output = args.test_dir.resolve()
    core, editor, unsupported, peers = discover(repository)
    components = {
        'matter_geometry_compute_off': {'backend': 'OFF', 'tier': 'cpu'},
        'matter_geometry_compute_on': {'backend': 'ON', 'tier': 'gpu'}}
    selected = set(args.only) if args.only else set(core) | set(editor) | set(unsupported) | set(components)
    failures = {name: unsupported.get(name, 'unknown CPU test')
                for name in selected if name not in core and name not in editor and name not in components}
    if failures:
        raise SystemExit(json.dumps({'unregistered': failures, 'peer_graphs': peers}, indent=2))
    core = {name: target for name, target in core.items() if name in selected}
    editor = {name: source for name, source in editor.items() if name in selected}
    components = {name: target for name, target in components.items() if name in selected}
    if args.archive_core:
        core = {name: dict(target, archive=True)
                for name, target in core.items()}
    standalone = {name: target for name, target in core.items()
                  if not target.get('archive')}
    archive = {name: target['source'] for name, target in core.items()
               if target.get('archive')}
    if args.build_core and standalone:
        directories = sorted({target['directory'] for target in standalone.values()})
        for directory in directories:
            recipes = sorted({target['build_target'] for target in standalone.values()
                              if target['directory'] == directory})
            subprocess.run(['make', '-C', str(repository / directory), '-j1',
                            *recipes], check=True)
    binaries = {name: repository / target['directory'] / target['binary']
                for name, target in standalone.items()}
    missing = {name: str(path) for name, path in binaries.items() if not path.is_file()}
    if missing:
        raise SystemExit(json.dumps({'missing_actual_binaries': missing}, indent=2))
    output.mkdir(parents=True, exist_ok=True)
    lines = ['cmake_minimum_required(VERSION 3.20)',
             'project(MatterCoreLinuxValidation LANGUAGES CXX)',
             'enable_testing()', 'set(CMAKE_CXX_STANDARD 17)',
             'set(CMAKE_CXX_STANDARD_REQUIRED ON)',
             'add_compile_options(-UNDEBUG)', 'find_package(Threads REQUIRED)']
    if archive:
        # Build the real canonical archive. Its Makefile is authoritative for
        # the production implementation closure, C/C++ flags and prerequisites.
        # CMake owns only the actual test TUs and final Linux linkage here.
        lines += ['find_program(MATTER_MAKE make REQUIRED)',
                  'add_custom_target(matter_linux_core_archive',
                  'COMMAND "${MATTER_MAKE}" -C ' + quote(repository / 'MatterEngine3') +
                  ' -j1 EXTRA_CFLAGS=-DMATTER_TEST_CACHE_VALIDATION_HOOK ' +
                  quote('GLSLC=' + args.glslc) + ' build/libmatter_engine3.a',
                  'BYPRODUCTS ' + quote(repository / 'MatterEngine3/build/libmatter_engine3.a') +
                  ' VERBATIM)',
                  'add_library(matter_linux_core STATIC IMPORTED GLOBAL)',
                  'set_target_properties(matter_linux_core PROPERTIES IMPORTED_LOCATION ' +
                  quote(repository / 'MatterEngine3/build/libmatter_engine3.a') + ')',
                  'add_dependencies(matter_linux_core matter_linux_core_archive)']
        includes = ['MatterEngine3', 'MatterEngine3/include', 'MatterEngine3/src',
                    'MatterEngine3/src/render', 'MatterEngine3/src/provider',
                    'MatterEngine3/tests', 'libs/MatterSurfaceLib/include',
                    'libs/MatterSurfaceLib/src', 'libs/SpatialQueryLib/include',
                    'libs/MemoryLib/include', 'libs/MathLib/include',
                    'libs/ProfileLib/include', 'libs/ParticleFlowLib/include',
                    'libs/MeshChartingLib/include', 'libs/AssetStoreLib/include',
                    'third_party/quickjs-ng', 'third_party/raylib/src',
                    'third_party/box3d/include', 'third_party/flecs',
                    'third_party/ozz-animation/include', 'third_party/bc7enc',
                    'third_party/Vulkan-Headers/include']
        dependencies = ['MatterEngine3/tests/build/box3d-linux/src/libbox3d.a',
                        'third_party/ozz-animation/build/matter/src/animation/offline/libozz_animation_offline.a',
                        'third_party/ozz-animation/build/matter/src/animation/runtime/libozz_animation.a',
                        'third_party/ozz-animation/build/matter/src/base/libozz_base.a',
                        'third_party/raylib/build/linux/libraylib.a']
        # Missing dependencies are real build failures; no platform stubs.
        lines += ['add_library(matter_linux_test_policy INTERFACE)',
                  'target_include_directories(matter_linux_test_policy INTERFACE ' +
                  ' '.join(quote(repository / item) for item in includes) + ')',
                  'target_compile_definitions(matter_linux_test_policy INTERFACE '
                  'MATTER_HAVE_SCRIPT_HOST MATTER_TEST_CACHE_VALIDATION_HOOK '
                  'PLATFORM_DESKTOP GRAPHICS_API_OPENGL_43 MATTER_VULKAN_ONLY)',
                  'target_link_libraries(matter_linux_test_policy INTERFACE matter_linux_core ' +
                  ' '.join(quote(repository / item) for item in dependencies) +
                  ' Threads::Threads m dl X11 GL rt)']
        qualification_sources = set(re.findall(
            r'matter_qualify_cpu_filesystem\(\w+\s+([^\s)]+)',
            (repository / 'cmake/MatterEngine.cmake').read_text()))
        legacy_qualifiers = {
            'world_definition_tests': 'qualify_world_sources.cpp',
            'part_asset_v2_tests': 'qualify_part_io.cpp',
            'partstore_tests': 'qualify_partstore_sources.cpp',
            'async_bake_tests': 'qualify_async_sources.cpp',
            'viewer_logic_tests': 'qualify_viewer_sources.cpp',
            'shader_source_tests': 'qualify_shader_sources.cpp'}
        generated_headers = set()
        for name, source in sorted(archive.items()):
            sources = core[name].get('sources', [source])
            lines += [f'add_executable({name} ' +
                      ' '.join(quote(repository / item) for item in sources) + ')',
                      f'target_link_libraries({name} PRIVATE matter_linux_test_policy)',
                      f'add_test(NAME {name} COMMAND {name})',
                      f'set_tests_properties({name} PROPERTIES LABELS "cpu;core" '
                      f'WORKING_DIRECTORY {quote(repository / "MatterEngine3/tests")})']
            if core[name].get('environment_qualification'):
                lines += ['find_package(Python3 REQUIRED COMPONENTS Interpreter)',
                          'include(' + quote(repository / 'libs/MatterGraphics/cmake/EnvironmentQualification.cmake') + ')',
                          f'matter_qualify_environment_test({name} "${{Python3_EXECUTABLE}}")',
                          f'set_tests_properties({name} PROPERTIES TIMEOUT 10)']
            if core[name].get('vulkan_cpu'):
                lines += ['find_library(MATTER_LINUX_VULKAN_LOADER NAMES vulkan libvulkan.so.1 REQUIRED)',
                          f'target_include_directories({name} PRIVATE ' + quote(repository / 'MatterEditor/src') + ')',
                          f'target_include_directories({name} PRIVATE ' + quote(repository / 'third_party/raylib/src/external/glfw/include') + ')',
                          f'target_compile_options({name} PRIVATE -ffunction-sections -fdata-sections)',
                          f'target_link_options({name} PRIVATE -Wl,--gc-sections)',
                          f'target_link_libraries({name} PRIVATE "${{MATTER_LINUX_VULKAN_LOADER}}")',
                          f'set_tests_properties({name} PROPERTIES PASS_REGULAR_EXPRESSION "ALL PASS")']
            definitions = core[name].get('compile_definitions', [])
            if definitions:
                lines += [f'target_compile_definitions({name} PRIVATE ' +
                          ' '.join(quote(item) for item in definitions) + ')']
            if name in {'shared_lib_tests', 'world_definition_tests', 'finite_surface_recipe_tests', 'solid_source_evaluation_tests', 'brick_bond_detail_tests', 'obj_export_golden_tests'}:
                export = name == 'obj_export_golden_tests'
                world = name not in {'shared_lib_tests', 'world_definition_tests', 'obj_export_golden_tests'}
                header = output / ('test-fixtures/embedded_export_fixtures.h' if export else
                                   'test-fixtures/embedded_world_modules.h' if world else
                                   'test-fixtures/embedded_shared_modules.h')
                inputs = sorted((repository / 'MatterEngine3/shared-lib').glob('*.js')) + sorted(
                    (repository / 'MatterEngine3/tests/shared-lib-fixtures').glob('*.js'))
                if world:
                    for folder in ['projects/world_demo/shared-lib',
                                   'projects/world_demo/objects/texturing/bricks',
                                   'projects/world_demo/objects/castle/materials']:
                        inputs += sorted((repository / folder).glob('*.js'))
                if world:
                    inputs.append(repository / 'projects/world_demo/objects/templates/WorldSector.js')
                if export:
                    inputs = [p for p in sorted((repository / 'MatterEngine3/tests/fixtures/export').iterdir())
                              if p.suffix in {'.obj', '.mtl', '.golden'}]
                generator = repository / 'MatterEngine3/tools/embed_shared_test_sources.py'
                # Several consumers share one generated output rule.
                if header not in generated_headers:
                    generated_headers.add(header)
                    lines += ['find_package(Python3 REQUIRED COMPONENTS Interpreter)',
                          'add_custom_command(OUTPUT ' + quote(header) +
                          ' COMMAND "${Python3_EXECUTABLE}" ' + quote(generator) +
                          ' --repository ' + quote(repository) + ' --output ' + quote(header) +
                          (' --export' if export else ' --world' if world else '') +
                          ' DEPENDS ' + quote(generator) + ' ' + ' '.join(quote(item) for item in inputs) + ' VERBATIM)']
                lines += [f'target_sources({name} PRIVATE ' + quote(header) + ')',
                          f'target_include_directories({name} PRIVATE ' + quote(header.parent) + ')']
            if name == 'sector_bake_tests':
                header = output / 'test-fixtures/terrain_test_modules.h'
                generator = repository / 'MatterEngine3/tests/embed_terrain_modules.py'
                inputs = [repository / item for item in [
                    'projects/world_demo/objects/templates/WorldSector.js',
                    'projects/world_demo/scenes/streaming/StreamMountain/StreamMountain.js',
                    'projects/world_demo/scenes/streaming/StreamMountain/objects/WorldSector.js']]
                for folder in ['projects/world_demo/shared-lib', 'MatterEngine3/shared-lib']:
                    inputs += sorted((repository / folder).glob('*.js'))
                lines += ['find_package(Python3 REQUIRED COMPONENTS Interpreter)',
                          'add_custom_command(OUTPUT ' + quote(header) +
                          ' COMMAND "${Python3_EXECUTABLE}" ' + quote(generator) +
                          ' --repo-root ' + quote(repository) + ' --output ' + quote(header) +
                          ' DEPENDS ' + quote(generator) + ' ' +
                          ' '.join(quote(item) for item in inputs) + ' VERBATIM)',
                          f'target_sources({name} PRIVATE ' + quote(header) + ')',
                          f'target_include_directories({name} PRIVATE ' + quote(header.parent) + ')']
            if name == 'script_host_tests':
                header = output / 'test-fixtures/script_test_modules.h'
                generator = repository / 'MatterEngine3/tests/EmbedScriptModules.cmake'
                inputs = [repository / 'MatterEngine3/shared-lib' / (item + '.js')
                          for item in ['particleflow', 'strands', 'rng']]
                lines += ['add_custom_command(OUTPUT ' + quote(header) +
                          ' COMMAND "${CMAKE_COMMAND}" ' +
                          quote('-DMATTER_SCRIPT_ROOT=' + str(repository)) + ' ' +
                          quote('-DMATTER_SCRIPT_HEADER=' + str(header)) + ' -P ' + quote(generator) +
                          ' DEPENDS ' + quote(generator) + ' ' +
                          ' '.join(quote(item) for item in inputs) + ' VERBATIM)',
                          f'target_sources({name} PRIVATE ' + quote(header) + ')',
                          f'target_compile_definitions({name} PRIVATE ' +
                          quote('MATTER_SCRIPT_TEST_MODULES_HEADER="' + str(header) + '"') + ')']
            if name in legacy_qualifiers:
                qualifier = name + '_source_qualifier'
                lines += [f'add_executable({qualifier} ' + quote(repository /
                          'MatterEngine3/tools' / legacy_qualifiers[name]) + ' ' +
                          ' '.join(quote(repository / item) for item in sources[1:]) + ')',
                          f'target_link_libraries({qualifier} PRIVATE matter_linux_test_policy)',
                          f'add_custom_target({name}_source_qualification ALL '
                          f'COMMAND $<TARGET_FILE:{qualifier}> DEPENDS {qualifier} '
                          f'WORKING_DIRECTORY {quote(repository / "MatterEngine3/tests")} VERBATIM)',
                          f'add_dependencies({name} {name}_source_qualification)']
            if source in qualification_sources:
                qualifier = name + '_filesystem_qualifier'
                lines += [f'add_executable({qualifier} {quote(repository / "MatterEngine3/tools/qualify_cpu_sources.cpp")} ' +
                          ' '.join(quote(repository / item) for item in sources[1:]) + ')',
                          f'target_compile_definitions({qualifier} PRIVATE ' +
                          quote('MATTER_QUALIFICATION_SOURCE="' + str(repository / source) + '"') + ')',
                          f'target_link_libraries({qualifier} PRIVATE matter_linux_test_policy)',
                          f'add_custom_target({name}_filesystem_qualification ALL '
                          f'COMMAND $<TARGET_FILE:{qualifier}> DEPENDS {qualifier} '
                          f'WORKING_DIRECTORY {quote(repository / "MatterEngine3/tests")} VERBATIM)',
                          f'add_dependencies({name} {name}_filesystem_qualification)']
                if definitions:
                    lines += [f'target_compile_definitions({qualifier} PRIVATE ' +
                              ' '.join(quote(item) for item in definitions) + ')']
    for name, path in sorted(binaries.items()):
        lines.extend([f'add_test(NAME {name} COMMAND {quote(path)})',
                      f'set_tests_properties({name} PROPERTIES LABELS "cpu;core" '
                      f'WORKING_DIRECTORY {quote(repository / "MatterEngine3/tests")})'])
    for name, sources in sorted(editor.items()):
        lines.append(f'add_executable({name} ' +
                     ' '.join(quote(repository / source) for source in sources) + ')')
        lines.append(f'target_include_directories({name} PRIVATE ' +
                     ' '.join(quote(repository / directory) for directory in
                              ['MatterEditor/src', 'MatterEngine3/include', 'MatterEngine3/src']) + ')')
        lines.append(f'target_link_libraries({name} PRIVATE Threads::Threads)')
        lines.extend([f'add_test(NAME {name} COMMAND {name})',
                      f'set_tests_properties({name} PROPERTIES LABELS "cpu;editor")'])
    if components:
        lines += ['include(' + quote(repository / 'cmake/MatterGeometrySupplement.cmake') + ')',
                  'set(Vulkan_INCLUDE_DIR ' + quote(repository / 'third_party/Vulkan-Headers/include') + ')',
                  'find_library(Vulkan_LIBRARY NAMES vulkan libvulkan.so.1 REQUIRED)']
        for name, target in sorted(components.items()):
            lines += [f'matter_add_geometry_compute_test({name} {target["backend"]} {target["tier"]} '
                      + quote(repository) + ' ' + quote(args.glslc) + ')']
            if target['tier'] == 'gpu':
                environment = []
                if args.vulkan_layer_dir:
                    environment.append('VK_LAYER_PATH=' + str(args.vulkan_layer_dir.resolve()))
                if args.vulkan_layer_library_dir:
                    environment.append('LD_LIBRARY_PATH=' + str(args.vulkan_layer_library_dir.resolve()))
                if environment:
                    lines += [f'set_tests_properties({name} PROPERTIES ENVIRONMENT ' +
                              quote(';'.join(environment)) + ')']
    (output / 'CMakeLists.txt').write_text('\n'.join(lines) + '\n')
    record = {'scope': 'partial' if args.only else 'common CPU and compute GPU graph',
              'tests': sorted(selected), 'peer_graphs': peers,
              'unsupported_outside_selection': unsupported,
              'core_binaries': {name: str(path) for name, path in binaries.items()},
              'editor_sources': editor, 'archive_sources': archive,
              'geometry_compute_graphs': components,
              'vulkan_layer_dir': str(args.vulkan_layer_dir.resolve()) if args.vulkan_layer_dir else None,
              'vulkan_layer_library_dir': str(args.vulkan_layer_library_dir.resolve()) if args.vulkan_layer_library_dir else None}
    (output / 'registration.json').write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(record, indent=2))


if __name__ == '__main__':
    main()
