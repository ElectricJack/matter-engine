include_guard(GLOBAL)

option(MATTER_ENABLE_HEAVY_VULKAN_SMOKE_TESTS
    "Register Vulkan smoke regression modes that exceed the quick-test budget" OFF)

function(matter_add_vulkan_smoke_test name mode)
    # Keep expensive integration coverage available without making an ordinary
    # CTest invocation allocate streaming-world resources or run long sequences.
    # Even with 1 MiB geometry reserves, 16 px impostor cells and one llvmpipe
    # worker, every named regression below exceeded 256 MiB in the Linux audit.
    # See docs/agent/evidence/2026-10-14-vulkan-smoke-budget.json.
    set(heavy_modes
        full geometry-pages vt-feedback vt-input-snapshot vt-direct-source
        vt-surfaces sparse-voxel vt-queue vt-normal-frame vt-material-domain
        vt-pom-work vt-receiver-material vt-module-residency vt-export
        vt-surface-connections vt-feedback-pair rt-empty-tlas water-field-nort)
    if(mode IN_LIST heavy_modes)
        if(NOT MATTER_ENABLE_HEAVY_VULKAN_SMOKE_TESTS)
            return()
        endif()
        set(labels "vulkan;heavy")
        set(timeout 180)
    else()
        set(labels "vulkan;smoke")
        set(timeout 10)
    endif()
    add_test(NAME "${name}" COMMAND vulkan_smoke_tests)
    set_tests_properties("${name}" PROPERTIES
        LABELS "${labels}"
        ENVIRONMENT "MATTER_VK_SMOKE_MODE=${mode}"
        TIMEOUT "${timeout}"
        PROCESSORS 1
        RESOURCE_LOCK matter_gpu
        PASS_REGULAR_EXPRESSION "ALL PASS"
        FAIL_REGULAR_EXPRESSION "validation errors: [1-9][0-9]*"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
endfunction()
