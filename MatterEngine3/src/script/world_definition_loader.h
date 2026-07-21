#pragma once

#include "matter/world_definition.h"

#include <regex>
#include <string>

namespace matter {

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
