# Matter patches to QuickJS-ng 0.10.0

The vendored version remains 0.10.0. Changes below are deliberately limited;
they do not constitute a complete upstream update or an audit of every VM OOM
path. Preserve the upstream license in `LICENSE` and the source headers.

## Parser bytecode allocation failure (steady-torrent-37.1)

Upstream reference: [quickjs-ng/quickjs commit
3c78c95de4ce50b25d57e97f06eb95042584c7c8](https://github.com/quickjs-ng/quickjs/commit/3c78c95de4ce50b25d57e97f06eb95042584c7c8),
"Cherry-pick OOM handling and memory leak fixes (#1069)". That commit credits
Fabrice Bellard and partially incorporates Bellard commits `db3d3f09` and
`3dc7ef1f`. Its emitter fixes remain in both audited upstream snapshots:

- `v0.17.0`: `6d46d07d04041b40f4f49eaa7fdebe44c314c699`.
- `master`: `c359cac36372e71daade8602908d29bdc259b41e`.

Nine emitter/cleanup hunks from that upstream commit are backported to
`quickjs.c`:

- `get_prev_opcode` returns `OP_invalid` when the bytecode buffer has an error.
  `emit_op` records a position before attempting allocation, so a failed
  initial allocation previously let `js_is_live_code`/`emit_goto` read NULL;
  failed growth could similarly leave the position beyond the valid bytes.
- `emit_atom` reserves the operand before duplicating its atom, avoiding a
  reference that no emitted instruction owns. This uses the original patch's
  `dbuf_realloc`, compatible with 0.10.0; newer upstream uses `dbuf_claim`.
- `new_label_fd`, `new_label`, `emit_goto`, the two `get_lvalue` paths,
  `var_object_test` and `instantiate_hoisted_definitions` propagate failed label
  allocations without indexing label `-1`. The error remains sticky even when
  a caller ignores the return value.
- `free_bytecode_atoms` checks that an atom operand exists before reading it
  while freeing partially emitted bytecode.

Unrelated string/regexp parsing, object/property helpers, module creation and
public API changes from the upstream commit are not included.

### Local completion of the failure path

`js_create_function` additionally rejects a bytecode buffer whose error flag is
set, throws OOM, and uses the existing function-tree cleanup before any variable
or label resolution. This is a **Matter addition**, absent from the two audited
upstream snapshots. The upstream emitter guards alone let a failed resize leave
a truncated instruction that `resolve_variables` subsequently read past its
allocation under GCC ASan. Do not remove this gate merely because an upstream
version contains the `get_prev_opcode` fix.

### Build integration

Every consumer, including the portable host, compiles these fixes directly from
the vendor source; there is no build-directory transformation.

### Regression and validation

`MatterEngine3/src/script/portable_host/tests/parser_oom_tests.c` exercises
the real parser with each growing realloc failed in turn, including bytecode,
label and variable buffers in branches, atom operands, loops and nested
functions. It requires exceptions for every injected parser failure, executes
successful bytecode, checks runtime reuse through `JS_Eval`, and checks
allocator balance after teardown. The test includes the vendor translation unit
to reach the private parser; allocation failures stop before subsequent compiler
passes so optional source-map allocation failures are not mistaken for parser
failures. No test hooks or exported symbols are added to the VM. It is registered as
`matter_host_quickjs_parser_oom` alongside the host controls and Node parity
suites. These tests do not claim exhaustive safety for arbitrary JavaScript or
all VM allocation sites. Exploratory full-compilation sweeps also exposed a
later `resolve_variables`/`resolve_labels` cleanup failure and an unchecked
filename-atom allocation in `js_new_function_def`. Those separate issues are
not fixed by the emitter backport itself; the local compiler fixes below address
them.

Run all portable-host suites under GCC AddressSanitizer, including leak
detection, and native MSVC using the commands in the portable host README.

Validated on 2026-10-11: all three suites passed with GCC AddressSanitizer
(`detect_leaks=1:halt_on_error=1`) and native MSVC RelWithDebInfo. The parser
regression exercises 53 allocation failures across its four fixtures on both
toolchains, plus successful compilation/execution and runtime recovery.


## Compiler passes and function initialization (fleet-apex-25)

These are **local Matter fixes**, not additional hunks attributed to the upstream
emitter backport. The audit compared the compiler paths with quickjs-ng commit
`c359cac36372e71daade8602908d29bdc259b41e`: that snapshot still has unchecked
filename acquisition and the unsafe later-pass cleanup addressed here.

- `js_new_function_def` acquires the filename atom before linking the function
  into its parent's child list. Failure frees the unlinked definition.
- `cutils.c` makes a DynBuf error stop subsequent reservations and writes even
  when the remaining capacity would suffice. Otherwise a failed operand write
  could be followed by smaller writes that create malformed bytecode.
- Variable resolution transfers each atom reference to output or frees it when
  the output write fails. Duplicated atoms follow the same rule. Errors raised
  on the input buffer remain visible after replacement. Closure, pseudo-variable
  and eval-binding allocation failures now propagate to function-tree cleanup.
- Label resolution reserves its output upper bound before transferring atoms
  or patching jumps. On relocation failure, output owns the consumed prefix;
  the original buffer owns the unconsumed suffix. Cleanup releases those two
  disjoint sets and every outstanding relocation entry.
- Optional compressed source-map allocation uses the non-throwing runtime
  allocator and discards an incomplete map. Successful compilation must not
  leave a pending OOM exception or an incomplete encoded debug record.
- Class and switch bytecode patches reject failed emission before writing
  placeholders. Class initializer/constructor and function constant-pool
  allocation failures propagate; a failed pool insertion releases its value.
  Parser scope-allocation failures stop parsing before any subsequent scope use.
- Speculative parenthesis lookahead clears its discarded lexical exception
  before rewinding. The real parse still reports persistent errors; a recovered
  scan must not leave a pending exception on successful compilation.

The portable host's vendor SHA-256 guard is updated; its generated backtrace
retention patch remains unchanged.

### Public compiler regression

`tests/compiler_oom_tests.c` registers `matter_host_quickjs_compiler_oom` against
the same QuickJS library used by the portable host. Injection starts only after
`JS_NewRuntime2` and `JS_NewContext`. Each fault-free baseline determines the
allocation count; subsequent fresh runtimes fail each growing realloc in turn,
then each malloc/calloc/growing realloc in turn. Shrinks and setup allocations
are excluded. The parser regression shares the allocator in `oom_allocator.h`.

The eleven fixtures cover branches, object atoms, the original loop reproducer,
nested functions, closures, dead code, with scopes, private class fields, direct
eval/arguments, switch labels and finally blocks. A direct DynBuf test checks
that failed writes stay stopped. Each compile failure must carry an exception.
Optional debug or recovered lookahead failures may compile successfully only with no pending exception and
bytecode identical to the baseline after stripping source/debug data; that code
must execute to the expected result. Every run checks context reuse and zero
live allocator blocks after teardown.

This is a bounded compiler regression, not an exhaustive VM OOM audit. It does
not inject runtime/context setup, arbitrary module loading or execution-time
allocation failures.

Validated on 2026-10-11: all four portable-host suites passed with GCC
AddressSanitizer (`detect_leaks=1:halt_on_error=1`) and native MSVC
RelWithDebInfo. On each toolchain the public compiler test exercises 407
growing-realloc failures and 739 malloc/calloc/growing-realloc failures, with
87 optional/recovered successes meeting the bytecode and exception checks.
The separate parser regression still covers 53 injected failures.

## Upstream bug backports: backtrace ownership and BigInt (steady-torrent-37)

Base: quickjs-ng 0.10.0 (`VERSION`, `CONFIG_VERSION="0.10.0"`). The hunks below
are targeted backports of the equivalent fixes in quickjs-ng master
(`quickjs.c` as shipped with v0.17.0). This is not a version bump: no other
upstream change, API or behaviour was taken, so every engine and SDK consumer
keeps 0.10.0 semantics apart from the corrected defects.

| File | Original SHA-256 | Patched SHA-256 (all patches in this file) |
| --- | --- | --- |
| `quickjs.c` | `58f2c432fea9304bd1e07f233151e04c08b91a54f0d680e076cfb2128535cbf8` | `b1a4ba5fa95732021abba8a7510e4cd9797da3efeb7a42fe7ab7dd57dd3b6f41` |

Each hunk is marked `quickjs-ng upstream fix` in the source.

1. `build_backtrace` — exception ownership under repeated OOM. Callers pass the
   pending exception (`rt->current_exception`) as a borrowed `error_val`. A
   second allocation failure inside the function calls `JS_Throw`, which frees
   that pending exception while the function still uses it to define `stack`
   (heap use-after-free). The function now holds its own reference
   (`error_obj = js_dup(error_val)`) for its whole body.
2. `js_limb_clz` — returns `JS_LIMB_BITS` for 0. `clz32(0)` is undefined and the
   top limb of a normalized BigInt can be a zero sign limb, so the binary-radix
   digit count of a positive value with a zero sign limb (for example
   `0xffffffffffffffffn.toString(16)`) produced a leading `0`.
3. `js_bigint_to_string1` (binary radices) — a digit straddling the top limb no
   longer reads `r->tab[r->len]` (out of bounds when the zero sign limb is
   skipped, e.g. `(2n**126n).toString(8)`).
4. `js_bigint_asUintN` (shared by `BigInt.asUintN`/`asIntN`) — `asUintN` of a
   negative value returned it unchanged whenever `bits` covered its storage
   (`BigInt.asUintN(64, 0xffffffffffffffffn) === -1n`, `asUintN(32, -1n) ===
   -1n`). Negative inputs are now sign-extended to `bits` and an unsigned
   result gains a zero sign limb (`js_bigint_extend`). Widths needing more
   than `JS_BIGINT_MAX_SIZE` limbs throw `RangeError` instead of truncating
   the limb count to `int`.

Consumers: every Make build compiles this file directly; the root CMake target
`matter_quickjs` (`cmake/MatterThirdParty.cmake`) declares
`MATTER_QUICKJS_BACKTRACE_OWNERSHIP_FIXED`, which
`MatterEngine3/src/script/portable_host` requires of a parent-supplied target.

Regressions: `MatterEngine3/src/script/portable_host/tests/host_tests.cpp`
(`HOST-BIGINT-CONFORMANCE-v1`, `HOST-BIGINT-VENDOR-v1`,
`HOST-OOM-BACKTRACE-OWNERSHIP-v1` under AddressSanitizer) and the
`HOST-SDK-seeded` frozen SplitMix64 vectors in `tests/parity.mjs`.

When updating the vendor, drop any hunk the new upstream already contains,
re-run those regressions, and update the hashes above.
