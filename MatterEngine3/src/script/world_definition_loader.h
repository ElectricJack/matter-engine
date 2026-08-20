#pragma once

// MatterEngine3/src/script/world_definition_loader.h — the world-load entry
// point.
//
// load_world_definition() evaluates a world's `.js` in a throwaway QuickJS
// runtime and returns the declarative WorldDefinition. Types (WorldLoadDesc,
// WorldDefinition, WorldLoadError) live in matter/world_definition.h; the
// implementation, and the full description of the script surface, the phased
// `defineMaterial`/`getProp`/`entity` globals, the shared-lib import rules and
// the extractor ordering, are in world_definition_loader.cpp's file header.
//
// The only other thing here is the class-name lookup below, which is shared
// rather than duplicated so the loader and ScriptHost can never disagree about
// which class in a source file is the World.

#include "matter/world_definition.h"

#include <regex>
#include <string>

namespace matter {

// Loads the world named by `desc` into `definition`. Returns false and fills
// `error` (message + world path + the authored property path) on any failure,
// leaving `definition` empty — a partial world is never returned.
//
// Not cheap and not reentrant-friendly: it reads files, spins up and tears down
// a whole JS runtime, and RESETS the material registry's dynamic tail before
// repopulating it from the world's defineMaterial() calls. Loading a second
// world therefore invalidates the first world's dynamic material handles.
bool load_world_definition(const WorldLoadDesc& desc,
                           WorldDefinition& definition,
                           WorldLoadError& error);

namespace world_script_detail {

// ScriptHost's field evaluator and the statics loader must select World classes
// identically. Keep this lexical lookup shared without coupling either evaluator
// to the other's runtime lifetime.
inline std::string find_world_class_name(const std::string& source) {
    static const std::regex re(
        "class\\s+([A-Za-z_$][A-Za-z0-9_$]*)\\s+extends\\s+World\\b");
    std::smatch match;
    return std::regex_search(source, match, re) ? match[1].str() : std::string{};
}

} // namespace world_script_detail
} // namespace matter
