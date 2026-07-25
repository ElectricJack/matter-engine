#pragma once
#include <cstdint>
#include <functional>
#include <vector>

namespace viewer {

// Represents a selected object — either an ECS entity or a baked root.
struct SelectedObject {
    enum Kind : uint8_t { Entity, BakedRoot };
    Kind kind = Entity;
    uint64_t id = 0;  // entity id for Entity, resolved_hash for BakedRoot

    bool operator==(const SelectedObject& o) const {
        return kind == o.kind && id == o.id;
    }
    bool operator!=(const SelectedObject& o) const { return !(*this == o); }
};

class SelectionSet {
public:
    // Replace the entire selection with a single object.
    void replace(const SelectedObject& obj);

    // Toggle an object in/out of the selection (Ctrl+click).
    void toggle(const SelectedObject& obj);

    // Extend selection to include a range (Shift+click).
    // `ordered_ids` is the current visual ordering of all objects in the tree.
    // Selects everything between the primary and the target.
    void extend_range(const SelectedObject& target,
                      const std::vector<SelectedObject>& ordered_ids);

    // Clear the entire selection.
    void clear();

    // The primary (most recently clicked) object. Returns nullptr if empty.
    const SelectedObject* primary() const;

    // All selected objects.
    const std::vector<SelectedObject>& items() const { return items_; }

    // Is a specific object selected?
    bool contains(const SelectedObject& obj) const;

    // Number of selected objects.
    size_t size() const { return items_.size(); }
    bool empty() const { return items_.empty(); }

    // Remove objects that no longer exist. Call once per frame.
    // `alive` returns true if the object still exists in the scene.
    void validate(std::function<bool(const SelectedObject&)> alive);

private:
    std::vector<SelectedObject> items_;
    int primary_index_ = -1;  // index into items_ of the primary selection
};

} // namespace viewer
