"""One serial paired experiment on verified immutable Windows source exports.

MATTER_NATIVE_PAIR_ROOT=/mnt/d/mv15-78a4 aq test -n 0 <this-file> -q -s
Set MATTER_NATIVE_PAIR_COMMIT to execute one verified source while the other
exports. A subsequent invocation for the other commit appends to the same
experiment; every build and smoke evidence directory must still be new.
Build failure stops the experiment; smoke assertion failures are collected for
all four runs. Existing evidence is never overwritten. A successful driver
assertion requires all four unchanged native smokes to pass.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time

ARTIFACTS = Path(os.environ.get('MATTER_NATIVE_PAIR_ROOT', '/mnt/d/mv15-78a4'))
DRIVERS = Path(__file__).resolve().parent
PS = '/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe'
REFS = ('58289c187ef2565c7d0532fe28a8cfb903023e44',
        '90a19c6698a03623da9a3e3c5235ad272531d462')


def win(path):
    return subprocess.check_output(['wslpath', '-w', str(path)], text=True).strip()


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def run_driver(name, arguments, output, timeout):
    driver = ARTIFACTS / name
    if not driver.exists():
        shutil.copyfile(DRIVERS / name, driver)
    assert driver.read_bytes() == (DRIVERS / name).read_bytes()
    command = [PS, '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', win(driver), *arguments]
    with output.open('x') as log:
        completed = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=timeout)
    return {'argv': command, 'driver_exit_code': completed.returncode, 'driver_log': str(output)}


def test_native_pair():
    export = read_json(ARTIFACTS / 'export-receipt.json')
    assert export['task'] == 'nimble-rapids-30' and export['claim_epoch'] == 15
    selected = os.environ.get('MATTER_NATIVE_PAIR_COMMIT')
    assert selected is None or selected in REFS
    wanted = (selected,) if selected else REFS
    sources = [next(row for row in export['sources'] if row['commit'] == ref) for ref in wanted]
    result_path = ARTIFACTS / 'native-pair-result.json'
    if result_path.exists():
        results = read_json(result_path)
        assert selected and results['status'] == 'partial', 'No unchanged experiment retry'
        assert results['task'] == export['task'] and results['claim_epoch'] == 15
        assert not any(row['commit'] == selected for row in results['builds'])
    else:
        results = {'task': export['task'], 'claim_epoch': 15, 'started_at': time.time(),
                   'status': 'running', 'builds': [], 'smokes': [],
                   'planned_runs': [{'commit': ref, 'mode': mode, 'status': 'unexecuted'}
                                    for ref in REFS for mode in ('vt', 'vt-rt')]}

    def save():
        result_path.write_text(json.dumps(results, indent=2) + '\n')

    save()
    for source in sources:
        ref = source['commit']
        assert source['archive_modes_and_extracted_blobs_match_git_tree']
        root = Path(source['source_root'])
        evidence = ARTIFACTS / (ref + '-build')
        assert not evidence.exists(), 'Preserve previous build; no unchanged retry'
        print(f'BUILD {ref}', flush=True)
        driver = run_driver('run_native_build.ps1',
                            ['-SourceRoot', win(root), '-EvidenceDirectory', win(evidence)],
                            ARTIFACTS / (ref + '-build-driver.txt'), 1900)
        receipt_path = evidence / 'build-receipt.json'
        receipt = read_json(receipt_path) if receipt_path.exists() else {'status': 'no_receipt'}
        results['builds'].append({'commit': ref, **driver, 'receipt': receipt})
        if driver['driver_exit_code'] != 0 or receipt.get('exit_code') != 0:
            results['status'] = 'build_blocked'
            save()
            raise AssertionError(f'Native build stopped: {ref}: {driver}: {receipt}')
        # Hash the freshly generated executable and every compiled shader artifact.
        products = [root / 'MatterEditor/build/cmake/windows-msvc/relwithdebinfo/vulkan_smoke_tests.exe']
        products += sorted((root / 'MatterEditor/build').rglob('*.spv'))
        hashes = []
        for product in products:
            with product.open('rb') as handle:
                sha = hashlib.file_digest(handle, 'sha256').hexdigest()
            hashes.append({'path': str(product.relative_to(root)), 'sha256': sha,
                           'bytes': product.stat().st_size})
        (ARTIFACTS / (ref + '-products.json')).write_text(json.dumps(hashes, indent=2) + '\n')
        save()
        for mode in ('vt', 'vt-rt'):
            smoke_dir = ARTIFACTS / (ref + '-' + mode)
            assert not smoke_dir.exists(), 'Preserve previous smoke; no unchanged retry'
            print(f'SMOKE {ref} {mode}', flush=True)
            driver = run_driver('run_native_smoke.ps1',
                                ['-SourceRoot', win(root), '-EvidenceDirectory', win(smoke_dir),
                                 '-ToolchainReceipt', win(evidence / 'toolchain.json'), '-Mode', mode],
                                ARTIFACTS / (ref + '-' + mode + '-driver.txt'), 270)
            receipt = read_json(smoke_dir / 'smoke-receipt.json')
            output = '\n'.join((smoke_dir / name).read_text(errors='replace')
                               for name in ('smoke.stdout.txt', 'smoke.stderr.txt'))
            row = {'commit': ref, 'mode': mode, **driver, 'receipt': receipt,
                   'failures': re.findall(r'^FAIL:.*$', output, flags=re.MULTILINE),
                   'validation_errors': re.findall(r'validation errors: (\d+)', output),
                   'all_pass': 'ALL PASS' in output}
            results['smokes'].append(row)
            next(run for run in results['planned_runs'] if run['commit'] == ref and
                 run['mode'] == mode)['status'] = receipt['status']
            save()
            if (receipt['status'] != 'completed' or receipt['exit_code'] not in (0, 1)
                    or not row['validation_errors']):
                results['status'] = 'runtime_blocked'
                save()
                raise AssertionError(f'Native smoke did not complete normally: {row}')
    results['status'] = 'completed' if len(results['smokes']) == 4 else 'partial'
    results['finished_at'] = time.time()
    save()
    assert len(results['smokes']) == (4 if not selected else len(results['builds']) * 2)
    selected_smokes = [row for row in results['smokes'] if row['commit'] in wanted]
    assert all(row['receipt']['exit_code'] == 0 and row['all_pass'] and
               row['validation_errors'] == ['0'] for row in selected_smokes), selected_smokes
