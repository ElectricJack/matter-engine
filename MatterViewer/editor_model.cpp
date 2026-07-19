#include "editor_model.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <unordered_map>

namespace viewer {

using matter::scene::SceneEditError;
using matter::scene::SceneEditResult;
using matter::scene::SceneEntityId;
using matter::scene::SceneRecord;

namespace {

std::string to_lower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

} // namespace

void EditorModel::refresh(const SceneCommands& commands) {
    std::vector<SceneRecord> records;
    if (commands.query_records) {
        records = commands.query_records();
    }
    if (commands.generation) {
        last_generation_ = commands.generation();
    }

    rebuild_hierarchy(records);

    if (has_selection() && !is_selection_valid()) {
        clear_selection();
    }
}

void EditorModel::set_filter(const std::string& filter) {
    filter_ = filter;
    apply_filter();
}

void EditorModel::select(SceneEntityId id) {
    selection_.id = id;
    selection_.world_generation = last_generation_;
}

void EditorModel::clear_selection() {
    selection_ = Selection{};
}

void EditorModel::rebuild_hierarchy(const std::vector<SceneRecord>& records) {
    all_rows_.clear();

    // Map from record id -> record, and parent id -> children ids.
    std::unordered_map<uint64_t, const SceneRecord*> by_id;
    std::unordered_map<uint64_t, std::vector<uint64_t>> children_of;
    std::vector<uint64_t> roots;

    by_id.reserve(records.size());
    for (const auto& record : records) {
        by_id[record.id.value] = &record;
    }
    for (const auto& record : records) {
        if (record.parent_id.value == 0) {
            roots.push_back(record.id.value);
        } else {
            children_of[record.parent_id.value].push_back(record.id.value);
        }
    }

    std::sort(roots.begin(), roots.end());
    for (auto& [parent_id, kids] : children_of) {
        std::sort(kids.begin(), kids.end());
    }

    // Preorder DFS from roots, filling depth. child_count is filled in a
    // second pass since a node's row is written before its children are
    // known in full for nested structures — but since children_of already
    // has full counts up front, we can fill it in the same pass.
    std::function<void(uint64_t, uint32_t)> visit = [&](uint64_t id, uint32_t depth) {
        const SceneRecord* record = by_id[id];
        HierarchyRow row;
        row.id = record->id;
        row.parent_id = record->parent_id;
        row.name = record->name;
        row.depth = depth;
        auto it = children_of.find(id);
        row.child_count = (it != children_of.end())
                               ? static_cast<uint32_t>(it->second.size())
                               : 0;
        all_rows_.push_back(row);

        if (it != children_of.end()) {
            for (uint64_t child_id : it->second) {
                visit(child_id, depth + 1);
            }
        }
    };

    for (uint64_t root_id : roots) {
        visit(root_id, 0);
    }

    apply_filter();
}

void EditorModel::apply_filter() {
    if (filter_.empty()) {
        filtered_rows_ = all_rows_;
        return;
    }

    std::string needle = to_lower(filter_);
    filtered_rows_.clear();
    for (const auto& row : all_rows_) {
        if (to_lower(row.name).find(needle) != std::string::npos) {
            filtered_rows_.push_back(row);
        }
    }
}

bool EditorModel::is_selection_valid() const {
    if (selection_.id.value == 0) {
        return false;
    }
    for (const auto& row : all_rows_) {
        if (row.id.value == selection_.id.value) {
            return true;
        }
    }
    return false;
}

SceneEditResult EditorModel::create_empty(const SceneCommands& commands) {
    if (!commands.create_empty) {
        return SceneEditResult{SceneEditError::InvalidTarget, {}};
    }
    SceneEditResult result = commands.create_empty("New Entity");
    if (result.error == SceneEditError::None) {
        select(result.created_id);
    }
    return result;
}

SceneEditResult EditorModel::duplicate_selected(const SceneCommands& commands) {
    if (!has_selection()) {
        return SceneEditResult{SceneEditError::EntityNotFound, {}};
    }
    if (!commands.duplicate) {
        return SceneEditResult{SceneEditError::InvalidTarget, {}};
    }
    SceneEditResult result = commands.duplicate(selection_.id);
    if (result.error == SceneEditError::None) {
        select(result.created_id);
    }
    return result;
}

SceneEditResult EditorModel::delete_selected(const SceneCommands& commands) {
    if (!has_selection()) {
        return SceneEditResult{SceneEditError::EntityNotFound, {}};
    }
    if (!commands.delete_entity) {
        return SceneEditResult{SceneEditError::InvalidTarget, {}};
    }
    SceneEditResult result = commands.delete_entity(selection_.id);
    if (result.error == SceneEditError::None) {
        clear_selection();
    }
    return result;
}

SceneEditResult EditorModel::reparent_selected(const SceneCommands& commands,
                                                SceneEntityId new_parent) {
    if (!has_selection()) {
        return SceneEditResult{SceneEditError::EntityNotFound, {}};
    }
    if (new_parent.value == selection_.id.value) {
        return SceneEditResult{SceneEditError::CycleDetected, {}};
    }
    if (!commands.reparent) {
        return SceneEditResult{SceneEditError::InvalidTarget, {}};
    }
    return commands.reparent(selection_.id, new_parent);
}

} // namespace viewer
