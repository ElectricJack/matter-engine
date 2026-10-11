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

The portable host still applies its separate backtrace ownership retention fix
through `MatterEngine3/src/script/portable_host/quickjs-compat.cmake`. Its checked
vendor SHA-256 is updated for these source changes; the retention transformation
is unchanged. Other consumers compile these emitter fixes directly from the
vendor source.

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
not fixed by this emitter backport; follow-up task `fleet-apex-25` tracks them.

Run all three portable-host suites under GCC AddressSanitizer, including leak
detection, and native MSVC using the commands in the portable host README.

Validated on 2026-10-11: all three suites passed with GCC AddressSanitizer
(`detect_leaks=1:halt_on_error=1`) and native MSVC RelWithDebInfo. The parser
regression exercises 53 allocation failures across its four fixtures on both
toolchains, plus successful compilation/execution and runtime recovery.
