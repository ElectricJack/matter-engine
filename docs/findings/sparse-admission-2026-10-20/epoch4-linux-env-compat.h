#pragma once
#include <cstdlib>
#ifndef _WIN32
inline int _putenv_s(const char* key, const char* value) {
    return value && *value ? setenv(key, value, 1) : unsetenv(key);
}
#endif
