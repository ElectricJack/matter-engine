cmake_minimum_required(VERSION 3.25)

project(MatterEngine2 LANGUAGES C CXX)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "The root CMake build currently supports only native Windows MSVC; use the existing Makefiles on Linux")
endif()

include(CTest)
include(cmake/MatterCompiler.cmake)
include(cmake/MatterTargets.cmake)
include(cmake/MatterThirdParty.cmake)
include(cmake/MatterShaders.cmake)

set(matter_vulkan_sdk_root "C:/VulkanSDK/1.4.357.0")
set(matter_vulkan_glslc "${matter_vulkan_sdk_root}/Bin/glslc.exe")
set(matter_python_launcher
    "$ENV{LOCALAPPDATA}/Programs/Python/Launcher/py.exe")
foreach(required_shader_tool IN ITEMS
        "${matter_vulkan_glslc}"
        "${matter_python_launcher}"
        "${matter_vulkan_sdk_root}/Include/vulkan/vulkan.h"
        "${matter_vulkan_sdk_root}/Lib/vulkan-1.lib"
        "${matter_vulkan_sdk_root}/Bin/VkLayer_khronos_validation.json")
    if(NOT EXISTS "${required_shader_tool}")
        message(FATAL_ERROR
            "Pinned Windows shader dependency was not found: ${required_shader_tool}")
    endif()
endforeach()
execute_process(
    COMMAND "${matter_python_launcher}" -3 --version
    RESULT_VARIABLE matter_python_result
    OUTPUT_VARIABLE matter_python_stdout
    ERROR_VARIABLE matter_python_stderr
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE
)
set(matter_python_version "${matter_python_stdout}${matter_python_stderr}")
if(NOT matter_python_result EQUAL 0 OR
        NOT matter_python_version MATCHES "^Python 3\\.13\\.14$")
    message(FATAL_ERROR
        "Pinned Python 3.13.14 is required through py -3; got '${matter_python_version}'")
endif()

set(matter_shader_source_dir "${CMAKE_SOURCE_DIR}/MatterEngine3/shaders_vk")
file(GLOB matter_vulkan_shader_stages
    CONFIGURE_DEPENDS
    LIST_DIRECTORIES false
    RELATIVE "${matter_shader_source_dir}"
    "${matter_shader_source_dir}/*.comp"
    "${matter_shader_source_dir}/*.vert"
    "${matter_shader_source_dir}/*.frag"
    "${matter_shader_source_dir}/*.rgen"
    "${matter_shader_source_dir}/*.rmiss"
    "${matter_shader_source_dir}/*.rchit"
    "${matter_shader_source_dir}/*.rahit"
)
list(SORT matter_vulkan_shader_stages)

# MatterEngine3/Makefile remains the canonical logical SPIR-V inventory during
# migration. Discover stages from disk for CMake dependency edges, then require
# their outputs (plus the one specialization) to match that list exactly.
file(READ "${CMAKE_SOURCE_DIR}/MatterEngine3/Makefile"
    matter_engine_makefile_text)
string(FIND "${matter_engine_makefile_text}" "VK_SPV =" matter_make_inventory_start)
if(matter_make_inventory_start EQUAL -1)
    message(FATAL_ERROR "MatterEngine3/Makefile has no VK_SPV inventory")
endif()
string(SUBSTRING "${matter_engine_makefile_text}"
    ${matter_make_inventory_start} -1 matter_make_inventory_tail)
string(FIND "${matter_make_inventory_tail}" "\n\n" matter_make_inventory_end)
if(matter_make_inventory_end EQUAL -1)
    message(FATAL_ERROR "MatterEngine3/Makefile VK_SPV inventory is unterminated")
endif()
string(SUBSTRING "${matter_make_inventory_tail}"
    0 ${matter_make_inventory_end} matter_make_inventory_block)
string(REGEX MATCHALL "build/shaders_vk/[A-Za-z0-9_.]+\\.spv"
    matter_make_spirv_paths "${matter_make_inventory_block}")
set(matter_make_spirv_inventory)
foreach(make_spirv_path IN LISTS matter_make_spirv_paths)
    get_filename_component(make_spirv_name "${make_spirv_path}" NAME)
    list(APPEND matter_make_spirv_inventory "${make_spirv_name}")
endforeach()
set(matter_discovered_spirv_inventory)
foreach(shader_stage IN LISTS matter_vulkan_shader_stages)
    list(APPEND matter_discovered_spirv_inventory "${shader_stage}.spv")
endforeach()
list(APPEND matter_discovered_spirv_inventory "raster_skin.vert.spv")
list(SORT matter_make_spirv_inventory)
list(SORT matter_discovered_spirv_inventory)
if(NOT matter_make_spirv_inventory STREQUAL matter_discovered_spirv_inventory)
    message(FATAL_ERROR
        "CMake shader discovery does not match MatterEngine3/Makefile VK_SPV. "
        "Make: '${matter_make_spirv_inventory}', discovered: "
        "'${matter_discovered_spirv_inventory}'")
endif()
matter_add_vulkan_shader_pipeline(
    PREFIX matter
    SOURCE_DIR "${matter_shader_source_dir}"
    OUTPUT_DIR "${CMAKE_BINARY_DIR}/MatterEngine3/shaders_vk"
    EMBEDDED_HEADER
        "${CMAKE_BINARY_DIR}/MatterEngine3/shaders_gen/embedded_spirv.h"
    GLSLC "${matter_vulkan_glslc}"
    PYTHON_LAUNCHER "${matter_python_launcher}"
    EMBED_SCRIPT "${CMAKE_SOURCE_DIR}/MatterEngine3/tools/embed_spirv.py"
    SHADERS ${matter_vulkan_shader_stages}
    SPECIALIZATIONS
        "raster_skin.vert|raster.vert|MATTER_SKINNED_VERTEX_INPUT"
)

include(cmake/MatterEngine.cmake)
include(cmake/MatterViewer.cmake)

if(BUILD_TESTING)
    add_subdirectory(cmake/tests/third_party_smoke)
endif()
