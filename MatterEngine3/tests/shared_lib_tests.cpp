#include "../include/module_resolver.h"
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } } while (0)

// ---- Task 1: import-specifier parsing -------------------------------------
static void test_parse_imports() {
    const std::string src =
        "import { lsystem } from 'shared-lib/lsystem';\n"
        "import {rng} from \"shared-lib/rng\";\n"
        "import * as V from 'shared-lib/vecmath.js';\n"
        "// import { fake } from 'shared-lib/not-real';  (commented out)\n"
        "const s = \"import { str } from 'shared-lib/string-literal'\";\n"
        "class Foo extends Part { build(p){} }\n";
    std::vector<std::string> specs = module_resolver::parse_import_specifiers(src);
    std::sort(specs.begin(), specs.end());
    CHECK(specs.size() == 3, "exactly three real import specifiers parsed");
    CHECK(specs.size() == 3 && specs[0] == "shared-lib/lsystem", "specifier 0 = lsystem");
    CHECK(specs.size() == 3 && specs[1] == "shared-lib/rng", "specifier 1 = rng");
    CHECK(specs.size() == 3 && specs[2] == "shared-lib/vecmath.js", "specifier 2 keeps .js as written");
}

int main() {
    test_parse_imports();
    if (failures == 0) printf("All shared_lib tests passed\n");
    return failures == 0 ? 0 : 1;
}
