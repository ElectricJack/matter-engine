---
name: dead-code
description: Find unreachable/unused C++ code across the engine, editor and libs by rebuilding the linker's reachability analysis from objdump. Use when asked what code is dead, unused, unreferenced, safe to delete, or to audit a subsystem before deleting it.
---

## The one thing to know first

**Do not try to get this answer from the linker.** Every project here compiles
`-ffunction-sections -fdata-sections` and links `-Wl,--gc-sections`, so
`-Wl,--print-gc-sections` looks like it should just print the dead functions.
It does not work on this toolchain. Verified 2026-08-19 on mingw-w64 GCC 16.1 /
GNU ld (PE/COFF): `--gc-sections` collects `.data$` and `.rdata$` sections but
**never `.text$` sections**. A planted, provably unreferenced function survives
into `editor.exe` and is never reported. The relink is also byte-for-byte the
same size as the shipping exe, which is the tell.

(The Makefile comment at `MatterEditor/Makefile` around the `--whole-archive`
block asserts that `--gc-sections` "still prunes genuinely unreachable code".
On the Windows target it does not prune code, only data.)

So this skill rebuilds the analysis by hand from `objdump`.

## How it works

`dead_code.py` reconstructs exactly what `--gc-sections` would do on ELF:

- `objdump -t` -> per object: every symbol, the section holding it, section sizes.
  With `-ffunction-sections`, each function is its own `.text$<mangled>` section.
- `objdump -r` -> per section: its relocations, i.e. everything it references.
- Mark from the real roots (entry point, `_GLOBAL__sub_I*` static initialisers,
  `.ctors`/`.CRT$*`, non-empty catch-all sections), then sweep.

Then it filters so the output means something:

- **COMDAT dedup** — an inline/template symbol is emitted into many TUs; it is
  dead only when *every* definition is unreachable.
- **authored-code filter** — most unreachable sections are `std::`/`flecs::`
  template instantiations and vendored single-header libs (`stb_image*`
  textually included into `tileset_gtex.cpp`). Those are compiler output, not
  source you can delete.
- **shape** — `orphan` (nothing in the link references it) vs `dead-cluster`
  (referenced only by other dead code). Both are dead; a dead-cluster must be
  deleted as a group.
- **elsewhere** — whether the identifier occurs in `tests/`, `prototypes/`, or
  `.js` script files, none of which the shipping binary links.

## Run it

The analyzer takes one complete link's inputs. Get them from the real link line
rather than guessing, and never overwrite the shipping binary:

```bash
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
cd MatterEditor
make --dry-run -W build/windows/obj/gizmo.o windows 2>&1 | grep -m1 "editor\.exe" > /tmp/linkcmd.txt
tr ' ' '\n' < /tmp/linkcmd.txt | grep -E '\.o$|\.a$' | sort -u > /tmp/inputs.txt
echo "../third_party/autoremesher_core/libautoremesher_core.a" >> /tmp/inputs.txt   # pulled via -L/-l
```

`-W <some object>` makes make *pretend* that object changed, so it prints the
link recipe without building or touching anything.

```bash
python3 ../.claude/skills/dead-code/dead_code.py \
    --inputs /tmp/inputs.txt \
    --repo "D:/Shared With Desktop/AI/matter-engine-cpp" \
    --out /tmp/dead.json --md /tmp/dead.md
```

Takes ~60 s for the editor (489 objects); most of that is the repo-wide
identifier cross-reference, which `--no-xref` skips.

## Validate before you trust it

The tool ships a ground-truth check — a TU with one referenced and one
unreferenced function. Run it whenever the toolchain or the script changes:

```bash
bash .claude/skills/dead-code/selftest.sh
```

It must report exactly one authored dead function, shape `orphan`, and
`SANITY unreachable-with-live-caller (must be 0): 0`. That sanity line is
printed on every real run too — if it is ever non-zero the graph is wrong
(usually static symbols of the same name colliding across objects) and the
results must not be reported.

## Reading the output

`SHAPE x ELSEWHERE` buckets, most to least actionable:

| bucket | meaning |
|---|---|
| `orphan` / `none` | nothing anywhere references it. Delete candidates. |
| `dead-cluster` / `none` | a self-contained island of dead code. Delete as a group. |
| `* / tests` | product-dead but test-alive. Deleting means deleting tests too. |
| `* / prototypes` | only `prototypes/` (retired, not in `build-all.sh`) uses it. |
| `* / script` | name appears in a `.js` file — check for a QuickJS binding before touching. |

Always spot-check a few candidates with `grep -rn -w <name>` before deleting.
Exclude `.worktrees/` from that grep — it contains full copies of the tree and
will show phantom "references" in every result.

## Deleting what it finds — read this first

**Unreachable is not the same as uncalled.** This is the single biggest trap,
proven the hard way on 2026-08-19: of 92 candidates deleted in one pass, the
linker rejected 36. At `-O2` a function whose every call site was **inlined**
still emits an out-of-line copy that nothing references. The copy is genuinely
dead machine code, but the source function is live and deleting it breaks the
build. The analysis answers "is this compiled body reachable", not "is this
source called".

So never delete straight from the report. Gate every candidate:

1. **Source-level call-site check.** Delete only if the identifier appears
   nowhere except its own definition and declaration. Beware two regex traps
   that both produced false "safe" verdicts here: a bare call statement
   `foo(x);` looks exactly like a declaration, and `void* Foo(` / 
   `std::vector<T> Foo(` have a `*` or `>` before the name.
2. **`assert()` under `NDEBUG`.** The editor builds `-DNDEBUG`, so calls that
   only appear inside `assert(...)` vanish from that binary and leave no edge.
   `vulkan_smoke_tests.exe` builds *with* asserts and will fail to compile.
   Always build both.
3. **The linker is the oracle.** Iterate: apply the set, build, add every
   `undefined reference` to a reject list, restore, re-apply. Two or three
   rounds converge.
4. **Test binaries are not in the editor link.** Build
   `vulkan_smoke_tests.exe` and the `MatterEngine3/tests` targets too.
5. **Skip special members.** Destructors and `operator=` that show as
   unreachable are usually implicit or defaulted — there is nothing to delete,
   and removing a user-declared one changes the type's semantics.

Removing a definition leaves an orphan declaration in the header; a
declared-but-undefined member is legal C++ so the build stays green and the
lie persists. Sweep the headers afterwards and re-check.

## Caveats

- The answer is relative to **one binary**. A symbol dead in `editor.exe` may be
  live in `vulkan_smoke_tests.exe` or a `MatterEngine3/tests` target; that is
  what the `elsewhere` column approximates. For a definitive multi-binary answer,
  run the tool once per link and intersect the dead sets.
- Requires a **fresh build** — it reads object files, so stale objects give stale
  answers. `make -C MatterEditor windows` first (see the `build` skill: the
  editor compiles engine sources into its own `libmatter_engine3_viewer.a`, so
  `make -C MatterEngine3` alone does not update what the editor links).
- Virtual functions are handled correctly (vtable `.rdata$_ZTV*` relocations are
  real edges), but anything dispatched purely by string name at runtime —
  QuickJS bindings, FIFO command handlers, property-system registrations — can
  look dead. Check the `script` bucket and the registration tables.
- Whole `.cpp` files that no Makefile compiles are invisible here (no object to
  read). `uncompiled.py` covers that second axis, but note the test suites build
  straight to exes without leaving `.o` files, so everything under `tests/`
  is a false positive for it.

## Manual-link gotchas (Windows)

If you replicate a link command by hand rather than through `make`:

- `bash` needs **`TMPDIR`** exported, not just `TMP`/`TEMP`. With only TMP/TEMP
  set, GCC still dies with `Cannot create temporary file in C:\WINDOWS\`.
  `platform.mk` only covers make recipes, not hand-run command lines.
- Strip the leading `ccache` from the recipe, or export `USERPROFILE` and
  `LOCALAPPDATA` — ccache fails outright without them.
- Redirect to a scratch output path. Never let a probe link overwrite
  `build/windows/editor.exe`.
