"""Escape glslc's absolute Windows dependency paths for CMake/Ninja.

glslc -MD leaves spaces unescaped. Same-drive relative shader paths avoid this,
but external builds on another drive must use absolute paths. Windows disallows
colons inside path components, so drive prefixes delimit these paths unambiguously.
Reject unexpected or missing dependencies rather than lose dependency edges.
"""
import argparse
from pathlib import Path
import re


def escape(value):
    return (value.replace('\\', '/').replace('$', '$$').replace('#', '\\#')
            .replace(' ', '\\ ').replace('\t', '\\\t').replace(':', '\\:'))


def normalize(content, exists=lambda name: Path(name).is_file()):
    target, dependencies = content.strip().split(': ', 1)
    starts = [m.start() for m in re.finditer(r'(?<!\S)[A-Za-z]:[/\\]', dependencies)]
    if not starts or starts[0] != 0:
        raise ValueError('Expected absolute Windows glslc dependency paths')
    ends = starts[1:] + [len(dependencies)]
    paths = [dependencies[start:end].strip() for start, end in zip(starts, ends)]
    for name in paths:
        if not exists(name):
            raise ValueError(f'Invalid glslc dependency: {name}')
    return escape(target) + ': ' + ' '.join(escape(name) for name in paths) + '\n'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('depfile', type=Path)
    args = parser.parse_args()
    args.depfile.write_text(normalize(args.depfile.read_text(encoding='utf-8')), encoding='utf-8')


if __name__ == '__main__':
    main()
