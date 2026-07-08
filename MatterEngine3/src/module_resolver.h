#pragma once
#include <cstdint>
#include <string>
#include <vector>

// QuickJS-free module-resolution + canonical source-fold for the shared script
// library (SP-7). Parses static `import ... from '<specifier>'` statements,
// resolves `shared-lib/<name>` specifiers to files under a fixed root, gathers
// transitively-imported sources, and folds them into one canonical byte buffer
// for compute_resolved_hash. Requires no running QuickJS.
namespace module_resolver {

// Bare specifiers (e.g. "shared-lib/lsystem") found in static import statements,
// in source order, deduplicated-not. String/comment-embedded "import" text is
// ignored. Only single/double-quoted `from '<spec>'` forms are matched.
std::vector<std::string> parse_import_specifiers(const std::string& source);

// Maps a bare "shared-lib/<name>" specifier to "<root>/<name>.js". A trailing
// ".js" in the specifier is accepted and not doubled. Returns false (fail-closed,
// with err set) for: non-"shared-lib/" specifiers, names containing "/" or "..",
// or a resolved path that does not exist as a readable file.
bool resolve_specifier(const std::string& specifier, const std::string& shared_lib_root,
                       std::string& out_path, std::string& err);

struct ResolvedModule {
    std::string specifier;  // canonical resolved specifier, e.g. "shared-lib/leaf"
    std::string source;     // full module source as read from disk
};

struct FoldResult {
    // The canonical folded byte buffer: part source first, then each transitively
    // imported module's full source, modules ordered by resolved specifier
    // (lexicographic byte sort). This is the source_bytes input to
    // compute_resolved_hash. A NUL (0x00) separator is written between each
    // segment so concatenation is unambiguous (no specifier can contain NUL).
    std::vector<char>        folded;
    // Resolved specifiers actually folded (sorted), for diagnostics/tests.
    std::vector<std::string> resolved_specifiers;
    // The {canonical specifier -> source} set actually gathered, sorted by
    // specifier. The QuickJS module loader serves source from THIS set at eval
    // time (never re-reading the filesystem) to keep determinism and the
    // no-file-access contract intact. Same ordering as resolved_specifiers.
    std::vector<ResolvedModule> modules;
};

// Parse the part's imports, transitively resolve + read every shared-lib module,
// and produce the canonical fold buffer. Returns false (err set) on any missing
// module, illegal specifier, or read failure (fail-closed). Cycles are handled by
// visiting each resolved specifier at most once.
bool fold_sources(const std::string& part_source, const std::string& shared_lib_root,
                  FoldResult& out, std::string& err);

} // namespace module_resolver
