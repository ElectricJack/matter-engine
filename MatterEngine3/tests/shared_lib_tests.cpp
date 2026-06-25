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

// ---- Task 2: specifier resolution -----------------------------------------
static void test_resolve_specifier() {
    const std::string root = "shared-lib-fixtures";
    std::string path, err;
    CHECK(module_resolver::resolve_specifier("shared-lib/aaa", root, path, err),
          "resolve shared-lib/aaa");
    CHECK(path == "shared-lib-fixtures/aaa.js", "aaa resolves to aaa.js under root");
    CHECK(module_resolver::resolve_specifier("shared-lib/bbb.js", root, path, err),
          "trailing .js accepted");
    CHECK(path == "shared-lib-fixtures/bbb.js", "bbb.js resolves to bbb.js");
    // missing file -> fail closed
    CHECK(!module_resolver::resolve_specifier("shared-lib/nope", root, path, err),
          "missing module fails closed");
    // non-shared-lib specifier rejected
    CHECK(!module_resolver::resolve_specifier("./relative", root, path, err),
          "relative specifier rejected");
    CHECK(!module_resolver::resolve_specifier("shared-lib/../escape", root, path, err),
          "path traversal rejected");
}

int main() {
    test_parse_imports();
    test_resolve_specifier();
    if (failures == 0) printf("All shared_lib tests passed\n");
    return failures == 0 ? 0 : 1;
}
