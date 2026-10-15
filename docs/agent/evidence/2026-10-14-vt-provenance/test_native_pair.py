"""Run the authorized unchanged native historical build, once per revision.

aq test -n 0 <this-file> -q -x
Uses fresh source exports made by export_native_pair.py. A failed build stops
the experiment; do not rerun into an existing build-evidence directory.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess

import pytest

ARTIFACTS = Path(os.environ.get('MATTER_NATIVE_PAIR_ROOT',
    '/mnt/d/matter-agent-artifacts/matter-engine-cpp/nimble-rapids-30/epoch8-230ea608'))
REFS = ('58289c187ef2565c7d0532fe28a8cfb903023e44',
        '90a19c6698a03623da9a3e3c5235ad272531d462')
PS = '/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe'


def win(path):
    return subprocess.check_output(['wslpath', '-w', str(path)], text=True).strip()


@pytest.mark.parametrize('ref', REFS, ids=['pre-move', 'move'])
def test_historical_native_build(ref):
    export = json.loads((ARTIFACTS / 'export-receipt.json').read_text())
    assert export['task'] == 'nimble-rapids-30' and export['claim_epoch'] == 8
    source = next(row for row in export['sources'] if row['commit'] == ref)
    assert source['archive_modes_and_extracted_blobs_match_git_tree']
    runner = ARTIFACTS / 'run_native_build.ps1'
    if not runner.exists():
        shutil.copyfile(Path(__file__).with_name('run_native_build.ps1'), runner)
    else:
        assert runner.read_bytes() == Path(__file__).with_name('run_native_build.ps1').read_bytes()
    evidence = ARTIFACTS / (ref + '-build')
    assert not evidence.exists(), 'Preserve the previous attempt; no unchanged retry'
    command = [PS, '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', win(runner),
               '-SourceRoot', win(Path(source['source_root'])), '-EvidenceDirectory', win(evidence)]
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=1900)
    (ARTIFACTS / (ref + '-build-driver.txt')).write_text(result.stdout)
    assert result.returncode == 0, result.stdout
    receipt = json.loads((evidence / 'build-receipt.json').read_text(encoding='utf-8-sig'))
    assert receipt['status'] == 'completed' and receipt['exit_code'] == 0, receipt
