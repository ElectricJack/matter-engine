#pragma once
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

} // namespace module_resolver
