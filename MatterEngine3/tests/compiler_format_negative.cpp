#include "matter/compiler.h"

void compiler_format_probe(const char*, ...) MATTER_PRINTF_FORMAT(1, 2);

void compiler_format_probe(const char*, ...) {}

int main() {
    compiler_format_probe("%d", "not-an-int");
    return 0;
}
