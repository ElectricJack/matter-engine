# Fixture: a stand-in JavaScript engine (FJS_* symbols).
add_library(fakejs STATIC "${CMAKE_CURRENT_LIST_DIR}/fakejs.cpp")
target_include_directories(fakejs PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
