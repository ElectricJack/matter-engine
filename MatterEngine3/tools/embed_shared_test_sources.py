#!/usr/bin/env python3
"""Embed the real shared-module fixtures at build time, preserving exact bytes."""
import argparse
from pathlib import Path
import re

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--repository', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--world', action='store_true', help='embed the finite/solid receiver import closure')
parser.add_argument('--export', dest='export', action='store_true', help='embed pinned OBJ/MTL/digest fixtures')
args = parser.parse_args()
if args.world and args.export:
    parser.error('--world and --export are exclusive')
files = {}
if args.export:
    for path in sorted((args.repository / 'MatterEngine3/tests/fixtures/export').iterdir()):
        if path.suffix in {'.obj', '.mtl', '.golden'}:
            files[path.name] = path.read_bytes()
elif args.world:
    pending = [
        'projects/world_demo/objects/templates/WorldSector.js',
        'projects/world_demo/objects/texturing/bricks/ClayBrickSurface.js',
        'projects/world_demo/objects/texturing/bricks/ClayBrickWallSurface.js',
        'projects/world_demo/objects/texturing/bricks/ClayBrickWallPath.js',
        'projects/world_demo/objects/castle/materials/CastleStoneSource.js',
        'projects/world_demo/objects/castle/materials/CastleBrickBondDetail.js',
        'projects/world_demo/shared-lib/clay_brick_maze_layout.js',
        'projects/world_demo/shared-lib/wall_weathering.js',
        'projects/world_demo/shared-lib/clay_brick_material.js']
    while pending:
        key = pending.pop()
        if key in files:
            continue
        data = (args.repository / key).read_bytes()
        files[key] = data
        for module in re.findall(rb"from\s*['\"]shared-lib/([^'\"]+)['\"]", data):
            name = re.sub(r'\.js$', '', module.decode('utf-8')) + '.js'
            if '/' in name or '..' in name:
                raise ValueError('invalid module fixture: ' + name)
            for root in ['projects/world_demo/shared-lib', 'MatterEngine3/shared-lib']:
                child = root + '/' + name
                if (args.repository / child).is_file():
                    pending.append(child)
                    break
            else:
                raise FileNotFoundError(name)
else:
    for folder, key_root in [('MatterEngine3/shared-lib', '../shared-lib'),
                             ('MatterEngine3/tests/shared-lib-fixtures', 'shared-lib-fixtures')]:
        for path in sorted((args.repository / folder).glob('*.js')):
            files[key_root + '/' + path.name] = path.read_bytes()
function = ('embedded_export_test_sources' if args.export else
            'embedded_world_test_sources' if args.world else 'embedded_shared_test_sources')
lines = ['#pragma once', '#include "matter/fixture_io.h"',
         'inline matter::SourceSnapshot::Files ' + function + '() {']
for index, (_, data) in enumerate(sorted(files.items())):
    # Separate short literals also support native MSVC's string-literal limit.
    chunks = [data[start:start + 2048] for start in range(0, len(data), 2048)]
    lines.append('std::string source_' + str(index) + ';')
    for chunk in chunks:
        encoded = ''.join('\\%03o' % byte for byte in chunk)
        lines.append('source_' + str(index) + '.append("' + encoded + '",' + str(len(chunk)) + ');')
lines.append('return {')
for index, (key, _) in enumerate(sorted(files.items())):
    lines.append('{"' + key + '", std::move(source_' + str(index) + ')},')
lines += ['};', '}']
content = '\n'.join(lines) + '\n'
args.output.parent.mkdir(parents=True, exist_ok=True)
if not args.output.exists() or args.output.read_text() != content:
    args.output.write_text(content)
