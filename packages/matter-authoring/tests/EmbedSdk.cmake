# The isolated-realm regression executes the real SDK source, supplied by its
# native test helper from compiled bytes. Tests never open source as fixture data.
set(sdk_header "#pragma once\n#include <cstddef>\nstruct SdkModule { const char *name; const unsigned char *source; size_t size; };\n")
set(sdk_entries "")
foreach(module index values parameters rng recipe legacy)
    get_filename_component(path "${CMAKE_CURRENT_LIST_DIR}/../${module}.mjs" ABSOLUTE)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
    file(READ "${path}" bytes HEX)
    string(REGEX REPLACE "(..)" "0x\\1," bytes "${bytes}")
    string(APPEND sdk_header "static const unsigned char sdk_${module}[] = {${bytes}0};\n")
    string(APPEND sdk_entries "{\"${module}.mjs\", sdk_${module}, sizeof(sdk_${module}) - 1},\n")
endforeach()
string(APPEND sdk_header "static const SdkModule sdk_modules[] = {${sdk_entries}};\n")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/sdk_sources.h" "${sdk_header}")
