#include "part_graph.h"
#include <cstdio>

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } } while (0)

int main() {
    using namespace part_graph;
    // Empty params canonicalize to the empty string.
    CHECK(serialize_params(Params{}) == "", "empty params -> empty string");

    // Numbers use %.17g; key/value joined with '=' and terminated with ';'.
    {
        Params p;
        p["size"] = ParamValue::number(1.5);
        CHECK(serialize_params(p) == "size=1.5;", "single number param");
    }
    // Keys are emitted in sorted order regardless of insertion order (std::map sorts,
    // but assert the contract explicitly so a future container swap can't break it).
    {
        Params p;
        p["zeta"]  = ParamValue::number(2);
        p["alpha"] = ParamValue::number(1);
        CHECK(serialize_params(p) == "alpha=1;zeta=2;", "keys sorted lexicographically");
    }
    // Bools and strings.
    {
        Params p;
        p["hollow"] = ParamValue::boolean_(true);
        p["name"]   = ParamValue::string_("rock");
        CHECK(serialize_params(p) == "hollow=true;name=rock;", "bool + string params");
    }
    // Equal params (different insertion order) produce identical canonical strings.
    {
        Params a; a["x"] = ParamValue::number(0.1); a["y"] = ParamValue::number(0.2);
        Params b; b["y"] = ParamValue::number(0.2); b["x"] = ParamValue::number(0.1);
        CHECK(serialize_params(a) == serialize_params(b), "order-independent canonical form");
    }

    if (failures == 0) printf("All part_graph tests passed\n");
    return failures == 0 ? 0 : 1;
}
