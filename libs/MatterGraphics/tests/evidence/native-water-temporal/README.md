# Matched cull failure evidence

Both raw logs exit 1 with four `WRITE_AFTER_WRITE` hazards and two teardown
assertion failures. They are preserved byte for byte, including line endings;
local `.gitattributes` prevents Git from normalizing them.

`reproduction.json` records source commits, exact original runner scripts,
commands, native options, toolchain, layer environment, raw-log hashes and the
handle-only comparison procedure. The comparison predicate passed; the two
underlying cull modes failed. `spirv-sha256.json` contains the common hashes of
all 82 candidate/reference shader outputs.

These generated evidence files over 50 lines contain captured diagnostics and
reproduction metadata, not new renderer implementation. The separately tracked
fix is `clear-crest-48.4.11`; it must obtain zero synchronization hazards and all
passing assertions without weakening validation.
