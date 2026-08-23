cmake_minimum_required(VERSION 3.25)

project(MatterEngine2 LANGUAGES C CXX)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "The root CMake build currently supports only native Windows MSVC; use the existing Makefiles on Linux")
endif()

include(CTest)
include(cmake/MatterCompiler.cmake)
include(cmake/MatterTargets.cmake)
include(cmake/MatterThirdParty.cmake)
include(cmake/MatterEngine.cmake)

if(BUILD_TESTING)
    add_subdirectory(cmake/tests/third_party_smoke)
endif()
