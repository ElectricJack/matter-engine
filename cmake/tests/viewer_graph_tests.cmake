cmake_minimum_required(VERSION 3.25)

get_filename_component(repository_root "${CMAKE_CURRENT_LIST_DIR}/../.." REALPATH)
set(test_root "${repository_root}/MatterEditor/build/cmake/viewer-graph-test")
set(assertion_hook "${test_root}/assert-viewer-graph.cmake")
file(REMOVE_RECURSE "${test_root}")
file(MAKE_DIRECTORY "${test_root}")

file(WRITE "${assertion_hook}" [=[
if(CMAKE_SOURCE_DIR STREQUAL MATTER_REPOSITORY_ROOT)
    function(assert_list_contains list_value required message_text)
        list(FIND list_value "${required}" found_index)
        if(found_index EQUAL -1)
            message(FATAL_ERROR "${message_text}: '${list_value}'")
        endif()
    endfunction()

    function(assert_list_excludes list_value forbidden message_text)
        list(FIND list_value "${forbidden}" found_index)
        if(NOT found_index EQUAL -1)
            message(FATAL_ERROR "${message_text}: '${list_value}'")
        endif()
    endfunction()

    function(matter_assert_viewer_graph)
        get_target_property(headless_core_sources matter_engine_core SOURCES)
        get_target_property(headless_surface_sources matter_engine_surface_objects SOURCES)
        list(LENGTH headless_core_sources headless_core_count)
        list(LENGTH headless_surface_sources headless_surface_count)
        if(NOT headless_core_count EQUAL 130 OR NOT headless_surface_count EQUAL 21)
            message(FATAL_ERROR
                "accepted headless graph changed: core=${headless_core_count}, surface=${headless_surface_count}")
        endif()
        get_target_property(headless_core_definitions matter_engine_core COMPILE_DEFINITIONS)
        get_target_property(headless_surface_definitions matter_engine_surface_objects COMPILE_DEFINITIONS)
        get_target_property(headless_links matter_engine_headless LINK_LIBRARIES)
        assert_list_excludes("${headless_core_definitions}" MATTER_HAVE_AUTOREMESHER
            "headless core must not enable viewer retopology")
        assert_list_excludes("${headless_surface_definitions}" MATTER_HAVE_AUTOREMESHER
            "headless surface must not enable viewer retopology")
        assert_list_excludes("${headless_links}" matter_autoremesher
            "headless archive must not link the viewer autoremesher")

        get_target_property(viewer_sources matter_engine_viewer_objects SOURCES)
        set(viewer_sources_unique ${viewer_sources})
        list(REMOVE_DUPLICATES viewer_sources_unique)
        list(LENGTH viewer_sources viewer_count)
        list(LENGTH viewer_sources_unique viewer_unique_count)
        if(MATTER_EXPECT_RETOPO)
            set(expected_viewer_count 171)
        else()
            set(expected_viewer_count 170)
        endif()
        if(NOT viewer_count EQUAL expected_viewer_count OR
                NOT viewer_unique_count EQUAL expected_viewer_count)
            message(FATAL_ERROR
                "viewer source census is wrong for retopo=${MATTER_EXPECT_RETOPO}; count=${viewer_count}, unique=${viewer_unique_count}, expected=${expected_viewer_count}")
        endif()
        set(viewer_c_count 0)
        foreach(source IN LISTS viewer_sources)
            if(source MATCHES "\\.c$")
                math(EXPR viewer_c_count "${viewer_c_count} + 1")
            endif()
        endforeach()
        if(NOT viewer_c_count EQUAL 3)
            message(FATAL_ERROR "viewer C source census changed: ${viewer_c_count}")
        endif()

        get_target_property(viewer_definitions matter_engine_viewer_objects COMPILE_DEFINITIONS)
        get_target_property(viewer_options matter_engine_viewer_objects COMPILE_OPTIONS)
        get_target_property(viewer_links matter_engine_viewer_objects LINK_LIBRARIES)
        foreach(required_definition IN ITEMS
                NDEBUG MATTER_VULKAN_VIEWER MATTER_VULKAN_ONLY
                MATTER_HAVE_SCRIPT_HOST MATTER_HAVE_STREAMLINE=0)
            assert_list_contains("${viewer_definitions}" "${required_definition}"
                "viewer definition is missing")
        endforeach()
        assert_list_excludes("${viewer_definitions}" GRAPHICS_API_OPENGL_43
            "viewer must not use the headless GL selector")
        foreach(option IN LISTS viewer_options)
            if(option MATCHES "MatterHeadlessConfig")
                message(FATAL_ERROR "viewer must not force-include MatterHeadlessConfig: '${viewer_options}'")
            endif()
        endforeach()
        assert_list_excludes("${viewer_links}" matter_engine_headless
            "viewer must not reuse the headless archive")
        if(MATTER_EXPECT_RETOPO)
            assert_list_contains("${viewer_definitions}" MATTER_HAVE_AUTOREMESHER
                "retopo-enabled viewer definition is missing")
            assert_list_contains("${viewer_links}" matter_autoremesher
                "retopo-enabled viewer link is missing")
        else()
            assert_list_excludes("${viewer_definitions}" MATTER_HAVE_AUTOREMESHER
                "retopo-disabled viewer retains definition")
            assert_list_excludes("${viewer_links}" matter_autoremesher
                "retopo-disabled viewer retains link")
        endif()
        foreach(required_link IN ITEMS
                matter_memory matter_math matter_spatial matter_profile
                matter_particle_flow matter_mesh_charting matter_asset_store
                matter_quickjs matter_flecs matter_box3d matter_ozz_offline
                matter_bc7enc matter_glfw matter_vulkan_sdk)
            assert_list_contains("${viewer_links}" "${required_link}"
                "viewer direct-object boundary link is missing")
        endforeach()
    endfunction()
    cmake_language(DEFER CALL matter_assert_viewer_graph)
endif()
]=])

if(NOT DEFINED ENV{VSINSTALLDIR})
    message(FATAL_ERROR "viewer_graph_tests.cmake must run after VsDevCmd")
endif()
if(NOT DEFINED MATTER_TEST_PYTHON OR NOT EXISTS "${MATTER_TEST_PYTHON}")
    message(FATAL_ERROR "MATTER_TEST_PYTHON must name the discovered Windows Python executable")
endif()
set(ninja "$ENV{VSINSTALLDIR}/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe")

foreach(retopo IN ITEMS ON OFF)
    string(TOLOWER "${retopo}" retopo_directory)
    set(build_dir "${test_root}/build-${retopo_directory}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            -S "${repository_root}"
            -B "${build_dir}"
            -G Ninja
            "-DCMAKE_MAKE_PROGRAM=${ninja}"
            -DBUILD_TESTING=OFF
            "-DMATTER_ENABLE_AUTOREMESHER=${retopo}"
            "-DMATTER_EXPECT_RETOPO=${retopo}"
            "-DMATTER_PYTHON_EXECUTABLE:FILEPATH=${MATTER_TEST_PYTHON}"
            "-DMATTER_REPOSITORY_ROOT=${repository_root}"
            "-DCMAKE_PROJECT_INCLUDE=${assertion_hook}"
        RESULT_VARIABLE configure_result
        OUTPUT_VARIABLE configure_stdout
        ERROR_VARIABLE configure_stderr
    )
    if(NOT configure_result EQUAL 0)
        message(FATAL_ERROR
            "Viewer graph configure failed for retopo=${retopo}.\nstdout:\n${configure_stdout}\nstderr:\n${configure_stderr}")
    endif()
endforeach()

message(STATUS "viewer graph preserves isolated 130+21 headless and coherent 170/171-source viewer boundaries")
