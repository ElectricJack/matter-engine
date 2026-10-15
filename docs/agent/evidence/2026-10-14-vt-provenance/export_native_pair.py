"""Export immutable historical test artifacts into a NEW authorized D: directory.

Usage: python3 export_native_pair.py <fresh-artifact-root>
No git refs, AQ workspace registrations or source files in the checkout change.
"""
import gzip
import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tarfile
import time

REPO = Path(__file__).resolve().parents[4]
REFS = ('58289c187ef2565c7d0532fe28a8cfb903023e44',
        '90a19c6698a03623da9a3e3c5235ad272531d462')


def digest_file(path):
    with path.open('rb') as handle:
        return hashlib.file_digest(handle, 'sha256').hexdigest()


def main():
    target = Path(sys.argv[1])
    target.mkdir(parents=True, exist_ok=False)
    receipt = {'task': 'nimble-rapids-30', 'claim_epoch': 8,
               'owner_session': '7a96c3ba-eca7-490e-a511-e142fff96347',
               'execution_authorization': 'msg-55fa5898a0d54a0e86e8e3be10694467',
               'checkout_head': subprocess.check_output(
                   ['git', 'rev-parse', 'HEAD'], cwd=REPO, text=True).strip(),
               'created_at': time.time(), 'artifact_root': str(target), 'sources': []}
    manifest = target / 'export-receipt.json'
    manifest.write_text(json.dumps(receipt, indent=2) + '\n')
    for ref in REFS:
        archive = target / (ref + '.tar')
        with archive.open('xb') as handle:
            subprocess.run(['git', 'archive', '--format=tar', ref], cwd=REPO,
                           stdout=handle, check=True)
        expected = {}
        raw = subprocess.check_output(['git', 'ls-tree', '-r', '-z', ref], cwd=REPO)
        for entry in raw.split(b'\0'):
            if not entry:
                continue
            meta, name = entry.split(b'\t', 1)
            mode, kind, oid = meta.decode().split()
            assert kind == 'blob', (name, kind)
            expected[os.fsdecode(name)] = {'git_mode': mode, 'git_blob': oid}
        root = target / ref
        root.mkdir(exist_ok=False)
        archived = {}
        with tarfile.open(archive) as handle:
            for member in handle.getmembers():
                if member.isdir():
                    continue
                assert member.name in expected, member.name
                assert not Path(member.name).is_absolute()
                assert '..' not in Path(member.name).parts
                mode = '120000' if member.issym() else (
                    '100755' if member.mode & 0o111 else '100644')
                assert mode == expected[member.name]['git_mode'], member.name
                archived[member.name] = member.mode
            assert set(archived) == set(expected)
            handle.extractall(root, filter='fully_trusted')
        inventory = []
        for name, info in expected.items():
            path = root / name
            metadata = path.lstat()
            if info['git_mode'] == '120000':
                assert stat.S_ISLNK(metadata.st_mode), name
                link = os.readlink(path)
                data = os.fsencode(link)
            else:
                assert stat.S_ISREG(metadata.st_mode), name
                link = None
                data = path.read_bytes()
            blob = hashlib.sha1(b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest()
            assert blob == info['git_blob'], name
            inventory.append({'path': name, **info,
                              'archive_mode': oct(archived[name]),
                              'extracted_mode': oct(stat.S_IMODE(metadata.st_mode)),
                              'symlink_target': link, 'bytes': len(data),
                              'sha256': hashlib.sha256(data).hexdigest()})
        inventory_path = target / (ref + '-inventory.json.gz')
        with gzip.open(inventory_path, 'xt') as handle:
            json.dump(inventory, handle, indent=2)
            handle.write('\n')
        source = {'commit': ref, 'tree': subprocess.check_output(
                      ['git', 'rev-parse', ref + '^{tree}'], cwd=REPO, text=True).strip(),
                  'source_root': str(root), 'archive': str(archive),
                  'archive_sha256': digest_file(archive),
                  'inventory': inventory_path.name,
                  'inventory_sha256': digest_file(inventory_path),
                  'tracked_files': len(inventory),
                  'symlinks': sum(row['symlink_target'] is not None for row in inventory),
                  'archive_modes_and_extracted_blobs_match_git_tree': True}
        receipt['sources'].append(source)
        manifest.write_text(json.dumps(receipt, indent=2) + '\n')
        print(json.dumps(source), flush=True)


if __name__ == '__main__':
    main()
