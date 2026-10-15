"""Export immutable historical test artifacts into a NEW authorized D: directory.

Usage: python3 export_native_pair.py <fresh-artifact-root>
Add --reuse-archives <old-artifact-root> for the authorized short b/m layout.
No git refs, AQ workspace registrations or source files in the checkout change.
"""
import gzip
import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess
import tarfile
import time

REPO = Path(__file__).resolve().parents[4]
REFS = ('58289c187ef2565c7d0532fe28a8cfb903023e44',
        '90a19c6698a03623da9a3e3c5235ad272531d462')


def digest_file(path):
    with path.open('rb') as handle:
        return hashlib.file_digest(handle, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('target', type=Path)
    parser.add_argument('--reuse-archives', type=Path)
    args = parser.parse_args()
    target = args.target
    target.mkdir(parents=True, exist_ok=False)
    receipt = {'task': 'nimble-rapids-30', 'claim_epoch': 8,
               'owner_session': '7a96c3ba-eca7-490e-a511-e142fff96347',
               'execution_authorization': 'msg-55fa5898a0d54a0e86e8e3be10694467',
               'checkout_head': subprocess.check_output(
                   ['git', 'rev-parse', 'HEAD'], cwd=REPO, text=True).strip(),
               'created_at': time.time(), 'artifact_root': str(target), 'sources': []}
    if args.reuse_archives:
        receipt['execution_authorization'] = 'msg-f8ee60b5f905490c860e418aae1581fe'
        receipt['placement_only_diagnostic'] = True
    manifest = target / 'export-receipt.json'
    manifest.write_text(json.dumps(receipt, indent=2) + '\n')
    hashes = ('e5245b259a6840223a0f1c107fc54cc8b24db94968c5a018ed49637ec965f7d7',
              '8533667cdee243f18cfad4d5d180d9cd7b3fe6110a72ea885b0928233a8bd744')
    for index, ref in enumerate(REFS):
        archive = (args.reuse_archives or target) / (ref + '.tar')
        if args.reuse_archives:
            assert digest_file(archive) == hashes[index], str(archive)
        else:
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
        root = target / (('b', 'm')[index] if args.reuse_archives else ref)
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
        if args.reuse_archives:
            old_root = args.reuse_archives / ref
            native = lambda path: subprocess.check_output(
                ['wslpath', '-w', str(path)], text=True).strip()
            source['previous_source_root'] = native(old_root)
            source['native_source_root'] = native(root)
            source['previous_source_root_length'] = len(native(old_root))
            source['native_source_root_length'] = len(native(root))
        receipt['sources'].append(source)
        manifest.write_text(json.dumps(receipt, indent=2) + '\n')
        print(json.dumps(source), flush=True)


if __name__ == '__main__':
    main()
