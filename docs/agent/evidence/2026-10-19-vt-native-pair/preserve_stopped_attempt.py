"""Inventory the stopped move export and copy bounded evidence into the repo.

Run once after the owned exporter has stopped; never use its partial tree as
build input. Full archives and source trees remain in the task-owned D: root.
"""
import gzip
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

OUT = Path(__file__).resolve().parent
REPO = OUT.parents[3]
ARTIFACTS = Path('/mnt/d/mv15-78a4')
MOVE = '90a19c6698a03623da9a3e3c5235ad272531d462'
BASE = '58289c187ef2565c7d0532fe28a8cfb903023e44'


def main():
    inventory = []
    raw = subprocess.check_output(['git', 'ls-tree', '-r', '-z', MOVE], cwd=REPO)
    for entry in raw.split(b'\0'):
        if not entry:
            continue
        meta, name = entry.split(b'\t', 1)
        mode, kind, oid = meta.decode().split()
        assert kind == 'blob'
        path = ARTIFACTS / 'm' / os.fsdecode(name)
        row = {'path': os.fsdecode(name), 'git_mode': mode, 'git_blob': oid}
        if not path.exists() and not path.is_symlink():
            row['status'] = 'absent'
        else:
            data = os.fsencode(os.readlink(path)) if path.is_symlink() else path.read_bytes()
            actual = hashlib.sha1(b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest()
            row.update(status='matches' if actual == oid else 'mismatches', bytes=len(data),
                       actual_git_blob=actual, extracted_mode=oct(path.lstat().st_mode))
        inventory.append(row)
    with gzip.open(OUT / 'move-stopped-inventory.json.gz', 'xt') as handle:
        json.dump(inventory, handle, indent=2)
        handle.write('\n')
    archive = ARTIFACTS / (MOVE + '.tar')
    with archive.open('rb') as handle:
        archive_sha = hashlib.file_digest(handle, 'sha256').hexdigest()
    receipt = {
        'task': 'nimble-rapids-30', 'claim_epoch': 15,
        'reason': 'Stopped owned exporter after native baseline wrapper exited 0xC000013A; no retry',
        'exporter_exit_code': 143,
        'move_commit': MOVE, 'move_source_root': str(ARTIFACTS / 'm'),
        'move_archive_sha256': archive_sha,
        'move_export_complete': False, 'move_used_as_build_input': False,
        'partial_inventory': 'move-stopped-inventory.json.gz',
        'counts': {status: sum(row['status'] == status for row in inventory)
                   for status in ('matches', 'mismatches', 'absent')},
        'all_four_smokes_unexecuted': True,
        'baseline_archive_and_verified_source_preserved': True,
        'historical_source_or_graph_changed': False,
    }
    (OUT / 'stopped-attempt-receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    for name in ('export-receipt.json', 'native-pair-result.json', BASE + '-inventory.json.gz',
                 BASE + '-build-driver.txt'):
        assert not (OUT / name).exists(), name
        shutil.copyfile(ARTIFACTS / name, OUT / name)
    shutil.copytree(ARTIFACTS / (BASE + '-build'), OUT / 'baseline-build')
    for path in (OUT / 'baseline-build').iterdir():
        path.chmod(0o644)
    shutil.copyfile('/tmp/nimble-epoch15-baseline-pytest.txt', OUT / 'baseline-pytest.txt')
    configure = ARTIFACTS / 'b/MatterEditor/build/cmake/windows-msvc/relwithdebinfo/CMakeFiles/CMakeConfigureLog.yaml'
    if configure.exists():
        shutil.copyfile(configure, OUT / 'CMakeConfigureLog.yaml')
    # Keep whitespace-bearing raw evidence byte exact without diff whitespace noise.
    for name in ('baseline-pytest.txt', 'baseline-build/toolchain.json'):
        path = OUT / name
        with gzip.open(str(path) + '.gz', 'xb') as handle:
            handle.write(path.read_bytes())
        path.unlink()
    print(json.dumps(receipt, indent=2))


if __name__ == '__main__':
    main()
