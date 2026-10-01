"""Run native checks against a frozen source manifest; retain raw logs and hashes."""
import hashlib
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

out = Path(__file__).resolve().parent
repo = out.parents[3]
prefix, *modes = sys.argv[1:]
assert re.fullmatch(r'[a-z0-9-]+', prefix)
sources = json.loads((out / f'{prefix}-sources.json').read_text())


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def changed():
    return [p for p, h in sources.items() if not (repo / p).is_file() or sha(repo / p) != h]


assert not changed(), changed()
checks_path = out / f'{prefix}-tests.json'
checks = json.loads(checks_path.read_text()) if checks_path.exists() else {'runs': []}
for mode in modes:
    assert re.fullmatch(r'[a-z0-9-]+', mode)
    cpu_modes = {'partstore': 'partstore_tests', 'flatten': 'part_flatten_tests', 'writer': 'asset_export_tests', 'chart': 'chart_atlas_tests', 'cpu': 'vt_residency_tests',
                 'recipe': 'finite_surface_recipe_tests', 'provider': 'part_surface_provider_tests',
                 'world': 'world_definition_tests', 'mountain': 'world_definition_tests', 'eval': 'eval_world_tests',
                 'surface': 'surface_field_tests', 'face-material': 'face_material_bake_tests',
                 'contact': 'world_definition_tests'}
    target = {**cpu_modes, 'compositor': 'vt_compositor_tests',
              'static-surface': 'static_surface_vt_tests'}.get(mode, 'vulkan_smoke_tests')
    binary = repo / f'MatterEditor/build/cmake/windows-msvc/relwithdebinfo/{target}.exe'
    before = sha(binary)
    command = '$env:MATTER_VK_VALIDATION="1"; $env:TMP="$env:LOCALAPPDATA\\Temp"; $env:TEMP=$env:TMP; '
    command += f'$env:MATTER_VK_SMOKE_MODE="{mode}"; $env:MATTER_VT_PROP_TEXELS_PER_METER=$null; '
    working_dir = repo if mode in ('recipe', 'provider') else repo / 'MatterEditor'
    if mode in ('world', 'mountain', 'contact', 'eval'): working_dir = repo / 'MatterEngine3/tests'
    arguments = {'mountain': ' --mountain-material', 'contact': ' --surface-contact'}.get(mode, '')
    command += f'& "./{os.path.relpath(binary, working_dir)}"{arguments}; exit $LASTEXITCODE'
    log = out / f'{prefix}-test-{mode}.log'
    start = time.monotonic()
    with log.open('w') as stream:
        result = subprocess.run([
            '/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe',
            '-NoProfile', '-Command', command], cwd=working_dir,
            stdout=stream, stderr=subprocess.STDOUT)
    text = log.read_text(errors='replace')
    row = {'mode': mode, 'command': command, 'exit': result.returncode,
           'seconds': time.monotonic() - start, 'binary_sha256': before,
           'binary_unchanged': before == sha(binary), 'source_changes': changed(),
           'all_pass': 'ALL PASS' in text,
           'zero_validation_errors': mode in cpu_modes or 'validation errors: 0' in text,
           'skipped_rt': 'ray tracing unavailable, skipping' in text,
           'log_sha256': sha(log)}
    row['passed'] = (not row['exit'] and row['all_pass'] and row['zero_validation_errors']
                     and not row['skipped_rt'] and not row['source_changes'] and row['binary_unchanged'])
    checks['runs'].append(row)
    checks_path.write_text(json.dumps(checks, indent=2) + '\n')
    print(json.dumps(row), flush=True)
    for line in text.splitlines():
        if re.search(r'FAIL|Error|composed (seam eye|bend angle|diagonal eye)=|validation errors:|ALL PASS', line):
            print(line, flush=True)
    if not row['passed']:
        sys.exit(1)
