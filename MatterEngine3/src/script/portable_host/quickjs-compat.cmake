# The pinned VM borrows the pending exception while building its backtrace.
# A second OOM replaces/frees that exception, then can_add_backtrace reads it.
# Retain it for this function's lifetime. Generate locally: do not edit the
# shared vendor checkout or silently accept a changed implementation.
function(matter_host_prepare_quickjs input output)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${input}")
  file(SHA256 "${input}" digest)
  if(NOT digest STREQUAL "58f2c432fea9304bd1e07f233151e04c08b91a54f0d680e076cfb2128535cbf8")
    message(FATAL_ERROR "QuickJS changed: re-audit portable host backtrace ownership compatibility fix")
  endif()
  file(READ "${input}" source)
  string(REPLACE "rt->in_build_stack_trace = true;"
    "rt->in_build_stack_trace = true;\n    JS_DupValue(ctx, error_val); /* portable host: retain across nested OOM */"
    source "${source}")
  string(REPLACE "rt->in_build_stack_trace = false;"
    "JS_FreeValue(ctx, error_val); /* portable host: release retained exception */\n    rt->in_build_stack_trace = false;"
    source "${source}")
  file(WRITE "${output}" "${source}")
endfunction()
