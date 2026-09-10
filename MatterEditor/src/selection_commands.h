#pragma once

// Typed agent-selection command rules.  This stays header-only and depends
// only on the existing selection/inventory boundary so the atomicity and
// primary rules can be exercised without an editor, ECS or renderer.

#include "agent_protocol.h"
#include "scene_inventory.h"
#include "selection_set.h"

#include <algorithm>
#include <string>
#include <vector>

namespace viewer::selection_command {

enum class Operation { Replace, Add, Remove, Toggle, Clear, List };

inline const char* name(Operation operation) {
    switch (operation) {
        case Operation::Replace: return "replace";
        case Operation::Add: return "add";
        case Operation::Remove: return "remove";
        case Operation::Toggle: return "toggle";
        case Operation::Clear: return "clear";
        case Operation::List: return "list";
    }
    return "unknown";
}

struct ApplyResult {
    agent::Status status = agent::Status::Ok;
    std::string message;
    bool changed = false;
};

inline bool same(const agent::ObjectIdentity& a, const agent::ObjectIdentity& b) {
    return a.kind == b.kind && a.id == b.id;
}

inline SelectedObject selected(const agent::ObjectIdentity& object) {
    return {object.kind == agent::ObjectIdentity::Kind::BakedRoot
                ? SelectedObject::BakedRoot
                : SelectedObject::Entity,
            object.id};
}

// Parses one required objects array.  Repeated object identities are rejected
// rather than given accidental order-dependent semantics (especially toggle).
inline bool parse_objects(const matter::jsondoc::Value& value,
                          std::vector<agent::ObjectIdentity>& out,
                          std::string& error) {
    out.clear();
    if (value.kind != matter::jsondoc::Value::Kind::Array || value.arr.empty()) {
        error = "objects must be a non-empty array of object identities";
        return false;
    }
    out.reserve(value.arr.size());
    for (const matter::jsondoc::Value& value_item : value.arr) {
        agent::ObjectIdentity object;
        if (!agent::parse_object_identity(value_item, object, error)) return false;
        if (std::any_of(out.begin(), out.end(), [&](const auto& prior) {
                return same(prior, object);
            })) {
            error = "objects must not contain a duplicate typed id";
            return false;
        }
        out.push_back(object);
    }
    return true;
}

inline matter::jsondoc::Value selection_json(const inventory::Snapshot& snapshot,
                                              const SelectionSet& selection,
                                              Operation operation,
                                              bool changed) {
    using Value = matter::jsondoc::Value;
    Value result;
    result.kind = Value::Kind::Object;
    Value objects;
    objects.kind = Value::Kind::Array;
    for (const SelectedObject& item : selection.items()) {
        objects.arr.push_back(agent::object_identity_json(
            {item.kind == SelectedObject::BakedRoot
                 ? agent::ObjectIdentity::Kind::BakedRoot
                 : agent::ObjectIdentity::Kind::Entity,
             item.id}));
    }
    result.set("operation", [&] {
        Value value;
        value.kind = Value::Kind::String;
        value.str = name(operation);
        return value;
    }());
    result.set("changed", [&] {
        Value value;
        value.kind = Value::Kind::Bool;
        value.b = changed;
        return value;
    }());
    result.set("selection_revision", [&] {
        Value value;
        value.kind = Value::Kind::String;
        value.str = std::to_string(selection.revision());
        return value;
    }());
    result.set("scene_revision", [&] {
        Value value;
        value.kind = Value::Kind::String;
        value.str = std::to_string(snapshot.scene_revision);
        return value;
    }());
    result.set("objects", std::move(objects));
    const SelectedObject* primary = selection.primary();
    if (primary) {
        result.set("primary", agent::object_identity_json(
            {primary->kind == SelectedObject::BakedRoot
                 ? agent::ObjectIdentity::Kind::BakedRoot
                 : agent::ObjectIdentity::Kind::Entity,
             primary->id}));
    } else {
        result.set("primary", Value{});
    }
    return result;
}

// Validates every typed id against exactly one current snapshot BEFORE it
// derives or installs a new selection.  Thus a rebake/world switch/missing id
// cannot produce a partial mutation or retarget a reused numeric id.
inline ApplyResult apply(SelectionSet& selection,
                         const inventory::Snapshot& snapshot,
                         Operation operation,
                         const std::vector<agent::ObjectIdentity>& objects) {
    ApplyResult result;
    if (operation == Operation::Clear) {
        if (!objects.empty()) {
            result.status = agent::Status::InvalidInput;
            result.message = "selection.clear accepts no objects";
            return result;
        }
    } else if (operation == Operation::List || objects.empty()) {
        result.status = agent::Status::InvalidInput;
        result.message = "selection operation requires one or more objects";
        return result;
    }

    for (size_t i = 0; i < objects.size(); ++i) {
        if (!inventory::find_object(snapshot, objects[i])) {
            result.status = agent::Status::NotFound;
            result.message = "object at objects[" + std::to_string(i) +
                             "] does not exist at this scene revision";
            return result;
        }
    }

    std::vector<SelectedObject> desired = selection.items();
    const SelectedObject* old_primary = selection.primary();
    SelectedObject saved_primary{};
    const bool had_primary = old_primary != nullptr;
    if (had_primary) saved_primary = *old_primary;
    int primary = had_primary
                      ? static_cast<int>(std::find(desired.begin(), desired.end(),
                                                   saved_primary) - desired.begin())
                      : -1;

    const auto index_of = [&](const SelectedObject& object) {
        return std::find(desired.begin(), desired.end(), object);
    };
    if (operation == Operation::Replace) {
        desired.clear();
        for (const auto& object : objects) desired.push_back(selected(object));
        primary = static_cast<int>(desired.size()) - 1;
    } else if (operation == Operation::Clear) {
        desired.clear();
        primary = -1;
    } else if (operation == Operation::Add) {
        int newest_added = -1;
        for (const auto& object : objects) {
            const SelectedObject target = selected(object);
            if (index_of(target) != desired.end()) continue;
            desired.push_back(target);
            newest_added = static_cast<int>(desired.size()) - 1;
        }
        if (newest_added >= 0) primary = newest_added;
    } else if (operation == Operation::Remove) {
        for (const auto& object : objects) {
            const auto found = index_of(selected(object));
            if (found != desired.end()) desired.erase(found);
        }
        const auto still_primary = std::find(desired.begin(), desired.end(), saved_primary);
        primary = still_primary != desired.end()
                      ? static_cast<int>(still_primary - desired.begin())
                      : (desired.empty() ? -1 : static_cast<int>(desired.size()) - 1);
    } else {  // Toggle
        bool added = false;
        SelectedObject newest_added{};
        for (const auto& object : objects) {
            const SelectedObject target = selected(object);
            const auto found = index_of(target);
            if (found != desired.end()) {
                desired.erase(found);
            } else {
                desired.push_back(target);
                newest_added = target;
                added = true;
            }
        }
        if (added) {
            primary = static_cast<int>(
                std::find(desired.begin(), desired.end(), newest_added) - desired.begin());
        } else {
            const auto still_primary = std::find(desired.begin(), desired.end(), saved_primary);
            primary = still_primary != desired.end()
                          ? static_cast<int>(still_primary - desired.begin())
                          : (desired.empty() ? -1 : static_cast<int>(desired.size()) - 1);
        }
    }
    result.changed = selection.assign(std::move(desired), primary);
    return result;
}

}  // namespace viewer::selection_command
