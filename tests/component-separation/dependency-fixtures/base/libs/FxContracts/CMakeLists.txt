# Fixture: the neutral contracts component. Depends on nothing.
cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(fx_contracts LANGUAGES CXX)
endif()
add_library(fx_contracts STATIC src/fx_value.cpp)
target_include_directories(fx_contracts PUBLIC include)
target_compile_features(fx_contracts PUBLIC cxx_std_17)
