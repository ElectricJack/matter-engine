#pragma once

#if defined(_MSC_VER)
#include <intrin.h>
#define MATTER_FORCE_INLINE __forceinline
#define MATTER_ALIGN(N) __declspec(align(N))
#define MATTER_PRINTF_FORMAT(fmt_index, first_arg)
#define MATTER_CALLSITE_FILE __builtin_FILE()
#define MATTER_CALLSITE_LINE __builtin_LINE()
#elif defined(__GNUC__) || defined(__clang__)
#define MATTER_FORCE_INLINE inline __attribute__((always_inline))
#define MATTER_ALIGN(N) __attribute__((aligned(N)))
#define MATTER_PRINTF_FORMAT(fmt_index, first_arg) \
    __attribute__((format(gnu_printf, fmt_index, first_arg)))
#define MATTER_CALLSITE_FILE __builtin_FILE()
#define MATTER_CALLSITE_LINE __builtin_LINE()
#else
#define MATTER_FORCE_INLINE inline
#define MATTER_ALIGN(N)
#define MATTER_PRINTF_FORMAT(fmt_index, first_arg)
#define MATTER_CALLSITE_FILE __FILE__
#define MATTER_CALLSITE_LINE __LINE__
#endif

namespace matter {
namespace diagnostics {

MATTER_FORCE_INLINE void* return_address() noexcept {
#if defined(_MSC_VER)
    return _ReturnAddress();
#elif defined(__GNUC__) || defined(__clang__)
    return __builtin_extract_return_addr(__builtin_return_address(0));
#else
    return nullptr;
#endif
}

}  // namespace diagnostics
}  // namespace matter
