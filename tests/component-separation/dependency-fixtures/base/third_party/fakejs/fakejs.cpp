#include "fakejs.h"

extern "C" int FJS_Eval(const char* source) { return source ? 1 : 0; }
