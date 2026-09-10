#include "procedural_parameters.h"

#include <cmath>

namespace viewer::procedural {
namespace {
using matter::jsondoc::Value;

Value object() { Value v; v.kind = Value::Kind::Object; return v; }
Value array() { Value v; v.kind = Value::Kind::Array; return v; }
Value string(std::string s) { Value v; v.kind = Value::Kind::String; v.str = std::move(s); return v; }
Value boolean(bool b) { Value v; v.kind = Value::Kind::Bool; v.b = b; return v; }

const char* type_name(Value::Kind kind) {
    switch (kind) {
        case Value::Kind::Null: return "null";
        case Value::Kind::Bool: return "boolean";
        case Value::Kind::Number: return "number";
        case Value::Kind::UInt64: return "uint64";
        case Value::Kind::String: return "string";
        case Value::Kind::Array: return "array";
        case Value::Kind::Object: return "object";
    }
    return "unknown";
}

bool finite(const Value& value) {
    if (value.kind == Value::Kind::Number) return std::isfinite(value.num);
    if (value.kind == Value::Kind::Array)
        for (const Value& child : value.arr) if (!finite(child)) return false;
    if (value.kind == Value::Kind::Object)
        for (const auto& child : value.obj) if (!finite(child.second)) return false;
    return true;
}

bool compatible(const Value& current, const Value& next) {
    // JSON represents ordinary integral defaults as Number and only lifts
    // values beyond 2^53 to UInt64; accepting either preserves that contract.
    if ((current.kind == Value::Kind::Number || current.kind == Value::Kind::UInt64) &&
        (next.kind == Value::Kind::Number || next.kind == Value::Kind::UInt64))
        return true;
    return current.kind == next.kind;
}
}  // namespace

Value describe_json(const Root& root) {
    Value result = object();
    result.set("object", agent::object_identity_json(root.object));
    result.set("owner", string(root.module));
    Value source = object();
    source.set("available", boolean(!root.source_path.empty()));
    if (root.source_path.empty()) source.set("reason", string("the part graph recorded no source path"));
    else source.set("value", string(root.source_path));
    result.set("source", std::move(source));
    result.set("persistence", string("session_only_root_override"));
    Value params;
    if (!matter::jsondoc::parse_json(root.params_json, params) || params.kind != Value::Kind::Object) {
        result.set("available", boolean(false));
        result.set("reason", string("the published root parameters are not a JSON object"));
        return result;
    }
    result.set("available", boolean(true));
    Value fields = array();
    for (const auto& entry : params.obj) {
        Value field = object();
        field.set("name", string(entry.first));
        field.set("type", string(type_name(entry.second.kind)));
        field.set("default", entry.second);
        Value range = object();
        range.set("available", boolean(false));
        range.set("reason", string("static params record no portable range metadata"));
        field.set("range", std::move(range));
        fields.arr.push_back(std::move(field));
    }
    result.set("parameters", std::move(fields));
    return result;
}

bool make_plan(const Root& root, const Value& changes, Plan& out, std::string& error) {
    if (changes.kind != Value::Kind::Object || changes.obj.empty()) {
        error = "changes must be a non-empty JSON object";
        return false;
    }
    Value before;
    if (!matter::jsondoc::parse_json(root.params_json, before) || before.kind != Value::Kind::Object) {
        error = "the published root parameters are not a JSON object";
        return false;
    }
    Value after = before;
    std::vector<std::string> changed;
    for (const auto& entry : changes.obj) {
        const Value* declared = before.find(entry.first);
        if (!declared) { error = "unsupported parameter '" + entry.first + "'"; return false; }
        if (!finite(entry.second)) { error = "parameter '" + entry.first + "' must be finite"; return false; }
        if (!compatible(*declared, entry.second)) {
            error = "parameter '" + entry.first + "' must be " + type_name(declared->kind);
            return false;
        }
        const std::string old_json = matter::jsondoc::write_json(*declared);
        const std::string new_json = matter::jsondoc::write_json(entry.second);
        if (old_json != new_json) changed.push_back(entry.first);
        after.set(entry.first, entry.second);
    }
    out.root = root;
    out.before = std::move(before);
    out.after = std::move(after);
    out.changed = std::move(changed);
    return true;
}

Value plan_json(const Plan& plan, bool dry_run) {
    Value result = object();
    result.set("dry_run", boolean(dry_run));
    result.set("object", agent::object_identity_json(plan.root.object));
    result.set("owner", string(plan.root.module));
    result.set("persistence", string("session_only_root_override"));
    Value changed = array();
    for (const std::string& name : plan.changed) changed.arr.push_back(string(name));
    result.set("changed_parameters", std::move(changed));
    result.set("before", plan.before);
    result.set("after", plan.after);
    return result;
}
}  // namespace viewer::procedural
