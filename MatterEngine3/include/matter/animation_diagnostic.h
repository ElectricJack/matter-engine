#pragma once

#include <cstdint>
#include <string>

namespace matter {

// One animation authoring problem, flattened for reporting.
//
// This is the public, Matter-typed shape of animation::Diagnostic. It exists so a
// bake result, an editor panel, or an event subscriber can carry the FULL diagnostic
// list without any of them including the private animation IR headers (and therefore
// without dragging ozz anywhere near a public boundary).
//
// The private animation::Diagnostic nests its location in a SourceSpan; this flattens
// that, because every consumer here formats it rather than comparing it structurally.
struct AnimationDiagnostic {
    std::string code;     // stable identifier, e.g. "binding-validation"
    std::string message;  // author-facing text
    std::string module;   // source module, empty when the problem has no location
    uint32_t    line = 0; // 1-based; 0 means "no location"
    uint32_t    column = 0;
    std::string object;   // offending rig/joint/clip/input/target name, when known

    // Formatted "module:line:column (object)", or empty when there is no location.
    // Matches the shape BakeError::source_location already uses, so the two agree.
    std::string source_location() const {
        if (line == 0) return {};
        return module + ":" + std::to_string(line) + ":" + std::to_string(column) +
               " (" + object + ")";
    }
};

} // namespace matter
