#pragma once

// MatterEngine3/include/matter/json_doc.h
//
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

// A single JSON value. `kind` selects which member is live; the others keep
// their default. Two things are worth knowing before using it:
//
//   * `UInt64` exists only for integers a double cannot represent exactly.
//     The parser emits it when an unsigned integer literal exceeds 2^53
//     (content hashes, part ids) and the writer prints it losslessly.
//     Everything else — including every small integer — parses as `Number`.
//     A consumer that only looks at `num` will silently miss those values.
//   * Objects are a VECTOR of pairs, not a map. That is what preserves
//     insertion order, and the price is that key lookup is a linear scan and
//     duplicate keys are possible if you push into `obj` directly instead of
//     going through set().
struct Value {
    enum class Kind { Null, Bool, Number, UInt64, String, Array, Object } kind = Kind::Null;
    bool b = false;
    double num = 0.0;
    std::uint64_t uint64_value = 0;
    std::string str;
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj;

    // Linear scan of `obj`. Returns nullptr when the key is absent — which is
    // also what a non-object value returns, since its `obj` is empty. The
    // returned pointer is invalidated by any later set()/erase() on the same
    // value.
    Value* find(const std::string& key);
    const Value* find(const std::string& key) const;

    // Object helpers used by the property serializer. set() overwrites an
    // existing key IN PLACE (preserving its position) or appends a new one.
    Value& set(const std::string& key, Value v);
    bool erase(const std::string& key);
};

// Trailing garbage after the first complete value is tolerated.
// Returns false on malformed input; `out` may have been partially written by
// then, so treat it as unspecified rather than as a partial document.
bool parse_json(const std::string& text, Value& out);

// Same serializer, two shapes. The out-param overload APPENDS to `out` (it
// never clears it), which is what lets it recurse; clear the string yourself
// if you are reusing a buffer.
void write_json(const Value& v, std::string& out);
std::string write_json(const Value& v);

}  // namespace jsondoc
}  // namespace matter
