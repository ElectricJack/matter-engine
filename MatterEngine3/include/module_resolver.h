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

} // namespace module_resolver
