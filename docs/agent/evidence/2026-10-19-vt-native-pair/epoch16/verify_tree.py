import hashlib, os, subprocess, sys, stat
repo, ref, root = sys.argv[1:4]
raw = subprocess.check_output(['git', 'ls-tree', '-r', '-z', ref], cwd=repo)
bad = []; n = 0
for entry in raw.split(b'\0'):
    if not entry: continue
    meta, name = entry.split(b'\t', 1)
    mode, kind, oid = meta.decode().split()
    p = os.path.join(root, os.fsdecode(name))
    st = os.lstat(p)
    data = os.fsencode(os.readlink(p)) if stat.S_ISLNK(st.st_mode) else open(p, 'rb').read()
    if hashlib.sha1(b'blob %d\0' % len(data) + data).hexdigest() != oid: bad.append(name)
    n += 1
print(ref, 'tracked', n, 'mismatch', len(bad), bad[:5])
