#!/usr/bin/env python3
"""Second axis: whole source files that no build ever compiles.

The reachability pass only sees code that made it into an object file.  A .cpp
that no Makefile names is invisible to it -- and is dead in a much stronger
sense.  Compare every first-party source basename against the basenames of
every object produced by any build tree in the repo.
"""
import os, sys, collections

REPO = sys.argv[1] if len(sys.argv) > 1 else "."
SKIP = {".git", "node_modules", ".claude", ".worktrees", ".cache"}

srcs, objs = [], set()
for root, dirs, files in os.walk(REPO):
    dirs[:] = [d for d in dirs if d not in SKIP]
    rel = os.path.relpath(root, REPO).replace(os.sep, "/")
    inbuild = "/build" in "/" + rel or rel.startswith("build")
    for f in files:
        p = (rel + "/" + f).lstrip("./") if rel != "." else f
        if inbuild:
            if f.endswith(".o"):
                objs.add(os.path.splitext(f)[0])
            continue
        if f.endswith((".c", ".cpp", ".cc")) and not p.startswith("third_party/"):
            srcs.append(p)

never = [p for p in srcs
         if os.path.splitext(os.path.basename(p))[0] not in objs]
print("first-party sources        : %d" % len(srcs))
print("distinct built object names: %d" % len(objs))
print("sources with NO object anywhere: %d" % len(never))
print()
byd = collections.Counter(p.rsplit("/", 1)[0] for p in never)
for d, n in byd.most_common(30):
    print("  %-62s %d" % (d, n))
print()
for p in sorted(never):
    print("   ", p)
