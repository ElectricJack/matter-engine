#!/usr/bin/env python3
"""Whole-program dead-code analysis for the MatterEngine2 PE/COFF builds.

Why this exists
---------------
The obvious tool for this job is the linker: every project here compiles with
-ffunction-sections -fdata-sections and links with -Wl,--gc-sections, so
`-Wl,--print-gc-sections` should name every function the linker dropped.  It
does not.  On this mingw-w64 / GNU ld toolchain, --gc-sections collects
.data$/.rdata$ sections but NEVER .text$ sections -- a planted unreferenced
function survives into the binary and is not reported.  So the linker cannot
answer the question on Windows.

This script rebuilds the analysis the linker declines to do:

  * `objdump -t` gives, per object, every symbol and the section that holds it,
    plus every section's name and size.  With -ffunction-sections each function
    lives in its own `.text$<mangled>` section.
  * `objdump -r` gives, per section, the relocations it emits -- i.e. exactly
    the set of other sections/symbols that section references.
  * Mark from the program's real roots (entry point, static initialisers,
    non-empty catch-all sections) and sweep.  Anything unmarked is code the
    shipping binary cannot reach.

Reported results are then filtered so they mean something:
  * COMDAT dedup -- an inline/template symbol emitted into many TUs is dead
    only when EVERY definition of it is unreachable.
  * authored-code filter -- most unreachable sections are std::/flecs template
    instantiations and vendored single-header libs (stb_*), which are compiler
    output, not source you can delete.
  * in-edge shape -- 'orphan' (nothing references it at all) vs 'dead-cluster'
    (referenced only by other dead code).
  * cross-reference -- whether the identifier appears in tests/ or prototypes/,
    which the shipping binary does not link.
"""
import argparse, collections, json, os, re, subprocess, sys

# ---------------------------------------------------------------- objdump ---

SYMLINE = re.compile(
    r"^\[\s*\d+\]\(sec\s+(-?\d+)\)\(fl 0x..\)\(ty\s+\w+\)\(scl\s+(\d+)\)\s+"
    r"\(nx \d+\)\s+0x([0-9a-f]+)\s+(.*)$")
AUXLEN = re.compile(r"^AUX scnlen 0x([0-9a-f]+)")
FILEHDR = re.compile(r"^(\S.*?):\s+file format")
RELHDR = re.compile(r"^RELOCATION RECORDS FOR \[(.+)\]:")
RELROW = re.compile(r"^[0-9a-f]{8,16}\s+\S+\s+(.+)$")

UNWIND = (".pdata$", ".xdata$")
CATCHALL = (".text", ".data", ".rdata", ".bss", ".xdata", ".pdata")
ENTRY = ("main", "WinMain", "wWinMain", "mainCRTStartup",
         "WinMainCRTStartup", "DllMainCRTStartup", "DllMain")


class Obj:
    __slots__ = ("key", "path", "member", "src", "secname", "symsec",
                 "locals", "globals_", "seclen")

    def __init__(self, key, path, member):
        self.key, self.path, self.member = key, path, member
        self.src = None
        self.secname, self.symsec = {}, {}
        self.locals, self.globals_, self.seclen = {}, {}, {}


def key_for(path, member):
    return path if (member == path or path.endswith(member)) else path + "(" + member + ")"


def objdump(tool, args):
    return subprocess.run([tool] + args, capture_output=True, text=True,
                          errors="replace").stdout


def parse_symtab(tool, path, objs):
    cur, pending = None, None
    for line in objdump(tool, ["-t", path]).splitlines():
        m = FILEHDR.match(line)
        if m:
            k = key_for(path, m.group(1))
            cur = objs.get(k) or objs.setdefault(k, Obj(k, path, m.group(1)))
            pending = None
            continue
        if cur is None:
            continue
        m = SYMLINE.match(line)
        if m:
            sec, scl, name = int(m.group(1)), int(m.group(2)), m.group(4).strip()
            pending = None
            if scl == 103 or sec == -2:
                if cur.src is None and name and not name.startswith("."):
                    cur.src = name
                continue
            if sec <= 0:
                continue
            if name.startswith(".") and scl == 3:
                cur.secname[sec] = name
                cur.symsec.setdefault(name, sec)
                cur.locals.setdefault(name, sec)
                pending = sec
                continue
            cur.symsec[name] = sec
            (cur.globals_ if scl == 2 else cur.locals)[name] = sec
            continue
        m = AUXLEN.match(line)
        if m and pending is not None:
            cur.seclen[pending] = int(m.group(1), 16)
        pending = None


def parse_relocs(tool, path, objs, edges):
    cur, sec = None, None
    for line in objdump(tool, ["-r", path]).splitlines():
        m = FILEHDR.match(line)
        if m:
            cur, sec = objs.get(key_for(path, m.group(1))), None
            continue
        m = RELHDR.match(line)
        if m:
            sec = m.group(1)
            continue
        if cur is None or sec is None:
            continue
        m = RELROW.match(line)
        if not m:
            continue
        v = m.group(1).strip().split("+")[0].split("-0x")[0].strip()
        if "[" in v:
            v = v.split("[")[0]
        if v:
            edges[(cur.key, sec)].add(v)


# ------------------------------------------------------------ source index ---

def build_index(repo, skip_dirs):
    idx = collections.defaultdict(list)
    for root, dirs, files in os.walk(repo):
        dirs[:] = [d for d in dirs if d not in skip_dirs]
        for f in files:
            if f.endswith((".c", ".cpp", ".cc")):
                rel = os.path.relpath(os.path.join(root, f), repo).replace("\\", "/")
                idx[f].append(rel)
    return idx


def classify_src(idx, src, vendor_prefixes):
    paths = idx.get(src, []) if src else []
    if not paths:
        return "unknown", None
    first = [p for p in paths if not p.startswith(vendor_prefixes)]
    if first:
        return "first_party", sorted(first, key=len)[0]
    return "third_party", sorted(paths, key=len)[0]


# ----------------------------------------------------------- name filtering ---

NOISE_PREFIX = ("std::", "__gnu_cxx::", "flecs::", "ozz::", "ImGui", "Im",
                "b3", "b2", "js_", "JS_", "ecs_", "glfw", "stbi", "stbiw",
                "stb_", "bc7", "rgbcx", "tinyexr", "miniz", "mz_", "qjs",
                "__tcf_", "_GLOBAL__")
NOISE_SUB = ("std::_Function_handler", "std::__", "__gnu_cxx::", "flecs::_::",
             "::_M_", "::_S_", "_Hashtable<", "_Rb_tree", "__detail::")
PRIM = {"void", "bool", "int", "unsigned", "char", "float", "double", "long",
        "short", "signed", "const", "auto"}


def strip_ret(dem):
    d = 0
    for i, c in enumerate(dem):
        if c in "<([":
            d += 1
        elif c in ">)]":
            d -= 1
        elif c == " " and d == 0:
            head = dem[:i]
            if head in PRIM:
                return strip_ret(dem[i + 1:])
            if re.fullmatch(r"[A-Za-z_][\w:<>,&* ]*", head) and "::" not in head:
                return dem[i + 1:]
            return dem
    return dem


def is_authored(dem):
    s = strip_ret(dem).lstrip("*& ")
    if any(n in s for n in NOISE_SUB):
        return False
    return not s.startswith(NOISE_PREFIX)


def bare_name(dem, mangled):
    if dem == mangled:
        return mangled
    # Strip the ABI tag FIRST.  Leaving it on produces names like
    # 'disk_path[abi:cxx11]', which fail the identifier check in xref() and
    # silently drop every std::string-returning function from the
    # cross-reference -- they then look unreferenced everywhere.
    dem = re.sub(r"\[abi:cxx11\]", "", dem)
    dem = re.sub(r"\s*\[clone [^\]]*\]", "", dem)
    s, depth, end = dem, 0, len(dem)
    for i in range(len(s) - 1, -1, -1):
        if s[i] == ')':
            depth += 1
        elif s[i] == '(':
            depth -= 1
            if depth == 0:
                end = i
                break
    s, out, d = s[:end], [], 0
    for c in s:
        if c == '<':
            d += 1
        elif c == '>':
            d -= 1
        elif d == 0:
            out.append(c)
    s = "".join(out)
    if "::" in s:
        s = s.split("::")[-1]
    return s.strip().split(" ")[-1]


# ------------------------------------------------------------------- main ---

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--inputs", required=True,
                    help="file listing .o/.a paths of one complete link")
    ap.add_argument("--repo", required=True)
    ap.add_argument("--cwd", default=None,
                    help="directory the link inputs are relative to")
    ap.add_argument("--out", required=True, help="output JSON")
    ap.add_argument("--md", default=None, help="output markdown report")
    ap.add_argument("--objdump", default="objdump")
    ap.add_argument("--vendor", default="third_party/,prototypes/")
    ap.add_argument("--skip-dirs",
                    default=".git,build,.cache,node_modules,.claude,obj,obj_viewer,.worktrees,shaders_gen")
    ap.add_argument("--no-xref", action="store_true",
                    help="skip the repo-wide identifier cross-reference")
    a = ap.parse_args()

    skip = set(a.skip_dirs.split(","))
    vendor = tuple(x for x in a.vendor.split(",") if x)
    if a.cwd:
        os.chdir(a.cwd)

    inputs = [l.strip() for l in open(a.inputs) if l.strip()]
    objs = {}
    for p in inputs:
        if os.path.exists(p):
            parse_symtab(a.objdump, p, objs)
        else:
            print("MISSING: " + p, file=sys.stderr)
    edges = collections.defaultdict(set)
    for p in inputs:
        if os.path.exists(p):
            parse_relocs(a.objdump, p, objs, edges)

    gsym = {}
    for o in objs.values():
        for n, sec in o.globals_.items():
            if not n.startswith("."):
                gsym.setdefault(n, (o.key, o.secname.get(sec, "?")))

    def node_of(objkey, name):
        o = objs.get(objkey)
        if o is None:
            return None
        if name in o.locals:
            return (objkey, o.secname.get(o.locals[name], "?"))
        if name in o.globals_:
            return (objkey, o.secname.get(o.globals_[name], "?"))
        return gsym.get(name)

    # roots
    roots = set()
    for n in ENTRY:
        if n in gsym:
            roots.add(gsym[n])
    for n, node in gsym.items():
        if n.startswith("_GLOBAL__sub_"):
            roots.add(node)
    for o in objs.values():
        for sec, nm in o.secname.items():
            if nm.startswith((".ctors", ".dtors", ".CRT")):
                roots.add((o.key, nm))
            if nm in CATCHALL and o.seclen.get(sec, 0) > 0:
                roots.add((o.key, nm))

    resolved = []
    for src_node, vals in edges.items():
        for v in vals:
            t = node_of(src_node[0], v)
            if t:
                resolved.append((src_node, t))

    seen, stack = set(), [r for r in roots if r]
    while stack:
        n = stack.pop()
        if n is None or n in seen:
            continue
        seen.add(n)
        objkey, sec = n
        o = objs.get(objkey)
        if o is None:
            continue
        if "$" in sec:
            _, _, sym = sec.partition("$")
            for pfx in (".text$", ".xdata$", ".pdata$", ".rdata$", ".data$", ".bss$"):
                sib = pfx + sym
                if sib != sec and sib in o.symsec:
                    if (objkey, sib) not in seen:
                        stack.append((objkey, sib))
        for v in edges.get(n, ()):
            t = node_of(objkey, v)
            if t and t not in seen:
                stack.append(t)

    # in-edges, ignoring a function's own unwind data
    indeg = collections.defaultdict(set)
    for src, dst in resolved:
        if src == dst:
            continue
        if src[0] == dst[0] and dst[1].startswith(".text$"):
            sym = dst[1][len(".text$"):]
            if src[1] in (".pdata$" + sym, ".xdata$" + sym):
                continue
        indeg[dst].add(src)

    idx = build_index(a.repo, skip)
    objinfo = {k: classify_src(idx, o.src, vendor) for k, o in objs.items()}

    # group by symbol identity: globals repo-wide, statics per object
    groups = collections.defaultdict(lambda: {"nodes": [], "live": 0, "size": 0})
    for k, o in objs.items():
        kind, path = objinfo[k]
        for i, n in o.secname.items():
            if not n.startswith(".text$"):
                continue
            s = n[len(".text$"):]
            gid = s if s in o.globals_ else (k + "	" + s)
            rec = groups[gid]
            rec["nodes"].append((k, n, kind, path))
            rec["size"] = max(rec["size"], o.seclen.get(i, 0))
            if (k, n) in seen:
                rec["live"] += 1

    dead = []
    for gid, rec in groups.items():
        if rec["live"]:
            continue
        fp = [x for x in rec["nodes"] if x[2] == "first_party"]
        if not fp:
            continue
        dead.append((gid, rec, fp))

    mang = [g.split("	")[-1] for g, _, _ in dead]
    dem = subprocess.run(["c++filt"], input="\n".join(mang), capture_output=True,
                         text=True, errors="replace").stdout.splitlines()
    dem += [""] * (len(mang) - len(dem))

    rows = []
    for (gid, rec, fp), d in zip(dead, dem):
        sym = gid.split("	")[-1]
        d = d or sym
        callers = set()
        for k, n, _, _ in rec["nodes"]:
            callers |= indeg.get((k, n), set())
        live_callers = [c for c in callers if c in seen]
        rows.append({
            "symbol": sym, "demangled": d, "path": fp[0][3], "src": fp[0][0],
            "size": rec["size"], "ndefs": len(rec["nodes"]),
            "authored": is_authored(d),
            "bare": bare_name(d, sym),
            "shape": "orphan" if not callers else "dead-cluster",
            "n_callers": len(callers), "n_live_callers": len(live_callers),
        })

    if not a.no_xref:
        xref(a.repo, skip, vendor, rows)
    else:
        for r in rows:
            r["elsewhere"] = "unknown"

    json.dump({"rows": rows,
               "stats": {"objects": len(objs), "reached": len(seen),
                         "roots": len(roots)}},
              open(a.out, "w"), indent=1)

    emit(rows, len(objs), len(seen), a.md)


TEXT_EXT = (".c", ".cpp", ".cc", ".h", ".hpp", ".js", ".mjs", ".json", ".md",
            ".mk", ".py", ".sh", ".comp", ".vert", ".frag", ".glsl", ".rgen",
            ".rchit", ".rmiss", ".txt")


def xref(repo, skip, vendor, rows):
    names = {r["bare"] for r in rows
             if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", r["bare"] or "")}
    if not names:
        for r in rows:
            r["elsewhere"] = "none"
        return
    pat = re.compile(r"\b(" + "|".join(sorted(map(re.escape, names), key=len,
                                              reverse=True)) + r")\b")
    hits = collections.defaultdict(collections.Counter)
    for root, dirs, files in os.walk(repo):
        dirs[:] = [d for d in dirs if d not in skip]
        for f in files:
            if not f.endswith(TEXT_EXT):
                continue
            rel = os.path.relpath(os.path.join(root, f), repo).replace("\\", "/")
            try:
                txt = open(os.path.join(root, f), errors="replace").read()
            except OSError:
                continue
            if rel.startswith("third_party/"):
                b = "third_party"
            elif "/tests/" in rel or rel.endswith(("_tests.cpp", "_test.cpp")):
                b = "tests"
            elif rel.startswith("prototypes/"):
                b = "prototypes"
            elif f.endswith((".js", ".mjs")):
                b = "script"
            else:
                b = "src"
            for m in pat.finditer(txt):
                hits[m.group(1)][b] += 1
    for r in rows:
        h = hits.get(r["bare"], collections.Counter())
        r["hits"] = dict(h)
        r["elsewhere"] = ("tests" if h.get("tests") else
                          "prototypes" if h.get("prototypes") else
                          "script" if h.get("script") else "none")


def emit(rows, nobj, nreached, mdpath):
    a = [r for r in rows if r["authored"]]
    print("objects analysed : %d" % nobj)
    print("sections reached : %d" % nreached)
    print("unreachable symbols defined in first-party code: %d" % len(rows))
    print("  compiler/vendored artifacts : %d" % (len(rows) - len(a)))
    print("  authored functions          : %d  (%.1f KB)"
          % (len(a), sum(r["size"] for r in a) / 1024.0))
    bad = [r for r in a if r["n_live_callers"]]
    print("SANITY unreachable-with-live-caller (must be 0): %d" % len(bad))
    print()
    c, kb = collections.Counter(), collections.Counter()
    for r in a:
        c[(r["shape"], r["elsewhere"])] += 1
        kb[(r["shape"], r["elsewhere"])] += r["size"]
    print("%-14s %-11s %6s %9s" % ("SHAPE", "ELSEWHERE", "N", "BYTES"))
    for k, n in sorted(c.items(), key=lambda kv: -kb[kv[0]]):
        print("%-14s %-11s %6d %9d" % (k[0], k[1], n, kb[k]))

    if not mdpath:
        return
    byfile = collections.defaultdict(list)
    for r in a:
        byfile[r["path"]].append(r)
    with open(mdpath, "w") as fh:
        fh.write("# Dead code report\n\n")
        fh.write("Unreachable from the shipping binary's entry point, "
                 "%d authored functions, %.1f KB.\n\n"
                 % (len(a), sum(r["size"] for r in a) / 1024.0))
        for pth, rs in sorted(byfile.items(),
                              key=lambda kv: -sum(x["size"] for x in kv[1])):
            fh.write("## %s  (%d, %d bytes)\n\n"
                     % (pth, len(rs), sum(x["size"] for x in rs)))
            for r in sorted(rs, key=lambda x: -x["size"]):
                fh.write("- `%s` — %d B, %s, elsewhere=%s\n"
                         % (r["demangled"], r["size"], r["shape"], r["elsewhere"]))
            fh.write("\n")


main()
