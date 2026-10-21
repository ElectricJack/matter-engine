# A relocatable static HTTP bundle. Preserve the module layout used by the
# runtime's default dependency URLs and the demo's contract fixture loader.
get_filename_component(root "${WEB_SOURCE}/../../.." ABSOLUTE)
set(bundle "${WEB_BINARY}/demo-bundle")
get_filename_component(module_dir "${WASM_MODULE}" DIRECTORY)
get_filename_component(module_stem "${WASM_MODULE}" NAME_WE)
set(wasm "${module_dir}/${module_stem}.wasm")
foreach(required "${WASM_MODULE}" "${wasm}" "${WEB_BINARY}/viewer/demo/app.mjs" "${WEB_BINARY}/runtime/index.mjs")
    if(NOT EXISTS "${required}")
        message(FATAL_ERROR "Demo bundle input missing: ${required}")
    endif()
endforeach()
file(MAKE_DIRECTORY "${bundle}/wasm" "${bundle}/bindings/geometry-c/wasm"
    "${bundle}/packages/matter-authoring" "${bundle}/libs/MatterContracts/examples"
    "${bundle}/integrations/web-procedural/viewer" "${bundle}/integrations/web-procedural/runtime")
file(COPY "${WEB_SOURCE}/demo" "${WEB_SOURCE}/vendor" DESTINATION "${bundle}/integrations/web-procedural/viewer")
file(COPY "${WEB_BINARY}/viewer/" DESTINATION "${bundle}/integrations/web-procedural/viewer/dist")
file(COPY "${WEB_BINARY}/runtime/" DESTINATION "${bundle}/integrations/web-procedural/runtime/dist")
file(COPY_FILE "${WASM_MODULE}" "${bundle}/wasm/matter_geometry_wasm.mjs" ONLY_IF_DIFFERENT)
file(COPY_FILE "${wasm}" "${bundle}/wasm/matter_geometry_wasm.wasm" ONLY_IF_DIFFERENT)
file(COPY "${root}/bindings/geometry-c/wasm/geometry.mjs" DESTINATION "${bundle}/bindings/geometry-c/wasm")
file(GLOB sdk "${root}/packages/matter-authoring/*.mjs")
file(COPY ${sdk} DESTINATION "${bundle}/packages/matter-authoring")
file(COPY "${root}/libs/MatterContracts/examples/contracts.mjs"
    "${root}/libs/MatterContracts/examples/repeated-assembly.asset.json"
    DESTINATION "${bundle}/libs/MatterContracts/examples")
# The root entry point preserves query controls, and works beneath any URL prefix.
file(WRITE "${bundle}/index.html" [=[<!doctype html>
<html lang="en"><meta charset="utf-8"><title>Matter procedural web view</title>
<link rel="icon" href="data:,">
<script>location.replace(new URL('integrations/web-procedural/viewer/demo/index.html' + location.search + location.hash, location.href));</script>
<a href="integrations/web-procedural/viewer/demo/index.html">Open the procedural demo</a>
</html>
]=])
message(STATUS "Standalone demo bundle: ${bundle}")
