#pragma once
#if !defined(__EMSCRIPTEN__) || !defined(__wasm32__)
#error "matter-wasm32-worker-v1 requires Emscripten wasm32"
#endif
#if defined(__EMSCRIPTEN_PTHREADS__) || defined(__wasm_simd128__) || defined(__wasm_relaxed_simd__)
#error "matter-wasm32-worker-v1 excludes pthreads and SIMD"
#endif
#if defined(__FAST_MATH__)
#error "matter-wasm32-worker-v1 excludes fast math"
#endif
#if defined(__cplusplus) && !defined(__EXCEPTIONS)
#error "MatterContracts requires internal exception handling"
#endif
