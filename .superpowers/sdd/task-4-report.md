## Fix round (post-hoc review)

### Finding 1 (fixed) — eval_world step 5: wrong eval strategy for module sources

The committed code at step 5 used `fold.folded` (a `std::vector<char>` NUL-separated hash
buffer) in a ternary with `source` (a `std::string`), causing a compile error.  Beyond the
type error, using `fold.folded` as eval source was semantically wrong: it is the concatenated
hashing buffer, not executable JS.

Fix: mirrored `eval_part_publish_class` exactly.  When `use_module = true`, `wrapped` (raw
`source` + class-publish suffix) is evaluated via `eval_part_as_module` so QuickJS resolves
`import` statements through the fold_store module loader.  When `use_module = false`, the
previous `JS_Eval(…, JS_EVAL_TYPE_GLOBAL)` path is used unchanged.

Also fixed: step 3 was creating the context with `JS_NewContext(rt)` instead of
`new_bake_context(rt, use_module)`.  The latter enables ES-module intrinsics when
`use_module = true`, matching `eval_part_publish_class`.

Changed: `MatterEngine3/src/script_host.cpp` steps 3 and 5 of `eval_world`.

### Finding 2 (already handled; confirmed by new test assertions)

The existing merge block (step 2) already reads `static params` from the World class and
overlays caller overrides via the `kMerge` snippet.  The test at lines 57–70 of
`eval_world_tests.cpp` (already committed) confirms that passing `"{}"` yields a program
identical to passing `{"worldSeed":42}` (the class default), and that passing
`{"worldSeed":99}` yields a different program.  No code change was needed for Finding 2.

### Verification

```
make -C MatterEngine3          → clean (warnings only, no errors)
make -C MatterEngine3/tests run-evalworld  → ALL PASS
```
