// Headless regression for repeated required-child controls in the Asset Browser.
// Each control keeps the visible label "Go"; the enclosing row identity must
// make its final ImGui identity unique without involving command routing.

#include "asset_browser_ids.h"

#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace {

int failures = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (cond) {                                                        \
            std::printf("  ok: %s\n", msg);                                \
        } else {                                                           \
            std::printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);  \
            ++failures;                                                    \
        }                                                                  \
    } while (0)

struct ChildRow {
    const char* module;
    const char* params_json;
    std::size_t occurrence;
};

std::string go_control_identity(const ChildRow& row) {
    return viewer::required_child_row_identity(
               row.module, row.params_json, row.occurrence) +
           "/Go";
}

bool all_unique(const std::vector<ChildRow>& rows) {
    std::set<std::string> identities;
    for (const ChildRow& row : rows) identities.insert(go_control_identity(row));
    return identities.size() == rows.size();
}

void test_literal_go_rows_are_unique() {
    std::printf("-- literal Go labels use distinct row scopes\n");
    const std::vector<ChildRow> rows = {
        {"Pebble", "{}", 0},
        {"Rock", "{}", 0},
        {"Twig", "{}", 0},
    };
    CHECK(all_unique(rows), "different child modules produce unique Go identities");
}

void test_repeated_module_parameters_are_unique() {
    std::printf("-- repeated modules include parameter identity\n");
    const std::vector<ChildRow> rows = {
        {"Pebble", R"({"seed":0,"size":2})", 0},
        {"Pebble", R"({"seed":1,"size":2})", 0},
        {"Pebble", R"({"seed":2,"size":2})", 0},
    };
    CHECK(all_unique(rows),
          "same module with different parameter JSON produces unique Go identities");
}

void test_exact_duplicates_use_occurrence() {
    std::printf("-- exact duplicates include occurrence identity\n");
    const std::vector<ChildRow> rows = {
        {"Pebble", R"({"seed":0,"size":2})", 0},
        {"Pebble", R"({"seed":0,"size":2})", 1},
        {"Pebble", R"({"seed":0,"size":2})", 2},
    };
    CHECK(all_unique(rows),
          "exact duplicate module/parameter rows remain unique by occurrence");
}

}  // namespace

int main() {
    std::printf("test_asset_browser_ids\n");
    test_literal_go_rows_are_unique();
    test_repeated_module_parameters_are_unique();
    test_exact_duplicates_use_occurrence();
    if (failures) {
        std::printf("FAILED (%d)\n", failures);
        return 1;
    }
    std::printf("ALL PASS\n");
    return 0;
}
