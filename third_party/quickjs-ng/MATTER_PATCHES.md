# Local patches to vendored quickjs-ng

Base: quickjs-ng 0.10.0 (`VERSION`, `CONFIG_VERSION="0.10.0"`). The hunks below
are targeted backports of the equivalent fixes in quickjs-ng master
(`quickjs.c` as shipped with v0.17.0). This is not a version bump: no other
upstream change, API or behaviour was taken, so every engine and SDK consumer
keeps 0.10.0 semantics apart from the corrected defects.

| File | Original SHA-256 | Patched SHA-256 |
| --- | --- | --- |
| `quickjs.c` | `58f2c432fea9304bd1e07f233151e04c08b91a54f0d680e076cfb2128535cbf8` | `d6735dc11c161da870690e03dbb6bce8ad315fd75efb5323bc19772c4f64b5d8` |

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
