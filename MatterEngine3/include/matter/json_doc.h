#pragma once

// Minimal order-preserving JSON document. Lifted verbatim (behavior-neutral)
// out of MatterEditor/src/part_workbench.cpp's anonymous namespace so the
// engine-side property system and the editor share one implementation.
//
// Two properties are load-bearing and must not drift:
//   * object key order is INSERTION order (never sorted) — the workbench
//     manifest and the params diff panel depend on stable key ordering;
//   * integral numbers print without a trailing ".0", so {"seed": 3}
//     round-trips as a JS integer rather than 3.0.
//
// This is deliberately NOT the canonical JSON used for bake-cache content
// hashing (MatterEngine3/src/part_graph.cpp's params_to_json/params_from_json).
// That one stays independent: the part content address depends on its exact
// bytes.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace matter {
namespace jsondoc {

struct Value {
    enum class Kind { Null, Bool, Number, UInt64, String, Array, Object } kind = Kind::Null;
    bool b = false;
    double num = 0.0;
    std::uint64_t uint64_value = 0;
    std::string str;
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj;

    Value* find(const std::string& key);
    const Value* find(const std::string& key) const;

    // Object helpers used by the property serializer. set() overwrites an
    // existing key IN PLACE (preserving its position) or appends a new one.
    Value& set(const std::string& key, Value v);
    bool erase(const std::string& key);
};

// Trailing garbage after the first complete value is tolerated.
bool parse_json(const std::string& text, Value& out);

void write_json(const Value& v, std::string& out);
std::string write_json(const Value& v);

}  // namespace jsondoc
}  // namespace matter
