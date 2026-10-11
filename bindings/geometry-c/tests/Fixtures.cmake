# C and JS consume exactly these committed recipes. The C consumer needs no
# filesystem/JSON dependency; literals are generated only in the build tree.
set(_requests "${CMAKE_CURRENT_LIST_DIR}/requests.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_requests}")
file(READ "${_requests}" _json)
set(_header "#pragma once\n")
foreach(name box extrude sphere placement csg process query)
    string(JSON request GET "${_json}" ${name})
    string(REPLACE "\\" "\\\\" request "${request}")
    string(REPLACE "\"" "\\\"" request "${request}")
    string(REPLACE "\n" "\\n\"\n\"" request "${request}")
    string(APPEND _header "static const char ${name}_request[] = \"${request}\";\n")
endforeach()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/requests.h" "${_header}")
