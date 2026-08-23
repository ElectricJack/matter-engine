#pragma once

#if defined(_MSC_VER)
#include <intrin.h>
#define MATTER_ALIGN(N) __declspec(align(N))
#define MATTER_PRINTF_FORMAT(fmt_index, first_arg)
#define MATTER_CALLSITE_FILE __builtin_FILE()
#define MATTER_CALLSITE_LINE __builtin_LINE()
#elif defined(__GNUC__) || defined(__clang__)
#define MATTER_ALIGN(N) __attribute__((aligned(N)))
#define MATTER_PRINTF_FORMAT(fmt_index, first_arg) \
    __attribute__((format(gnu_printf, fmt_index, first_arg)))
#define MATTER_CALLSITE_FILE __builtin_FILE()
#define MATTER_CALLSITE_LINE __builtin_LINE()
#else
#define MATTER_ALIGN(N)
#define MATTER_PRINTF_FORMAT(fmt_index, first_arg)
#define MATTER_CALLSITE_FILE __FILE__
#define MATTER_CALLSITE_LINE __LINE__
#endif

namespace matter::diagnostics {

inline void* return_address() noexcept {
#if defined(_MSC_VER)
    return _ReturnAddress();
#elif defined(__GNUC__) || defined(__clang__)
    return __builtin_return_address(0);
#else
    return nullptr;
#endif
}

}  // namespace matter::diagnostics
