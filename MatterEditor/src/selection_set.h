#pragma once

// MatterEditor/src/selection_set.h
//
// The editor's app-wide selection: what is currently picked in the viewport,
// the Scene tree, or via `viewer.reveal_part`, and which of those is the
// PRIMARY (the one the gizmo acts on and the one shift+click anchors from).
//
// A selection entry is deliberately not a pointer into the world. It is a
// tagged id — a SceneEntityId for an ECS entity, a resolved part hash for a
// baked root — so a selection can survive a rebake, outlive an object, and be
// compared and stored without any lifetime coupling. The cost of that is that
// entries can go stale, which is what `validate()` exists to sweep up once a
// frame.
//
// Deliberately dependency-free: no ImGui, no engine headers, no notifications.
// Everything that cares (the outline overlay, the gizmo, the pick code, the
// Properties panel) polls it. main.cpp owns the one instance; the world-switch
// seam clears it through SessionBinding's ClearModelsFn (session_binding.h).
// App thread only; nothing here is synchronized.

#include <cstdint>
#include <functional>
#include <vector>

namespace viewer {

// Represents a selected object — either an ECS entity or a baked root.
//
// `kind` decides what `id` means, so the two must always travel together: the
// same 64-bit number can be a valid entity id AND a valid part hash. Equality
// compares both, which is what makes a mixed selection of entities and baked
// roots safe in one vector. Trivially copyable and cheap to pass by value; it
// holds no reference to the world, so a SelectedObject stays well-formed after
// the thing it names is gone (see SelectionSet::validate).
struct SelectedObject {
    enum Kind : uint8_t { Entity, BakedRoot };
    Kind kind = Entity;
    uint64_t id = 0;  // entity id for Entity, resolved_hash for BakedRoot

    bool operator==(const SelectedObject& o) const {
        return kind == o.kind && id == o.id;
    }
    bool operator!=(const SelectedObject& o) const { return !(*this == o); }
};

// An ordered list of selected objects plus an index naming the primary.
//
// Invariant, re-established by every mutator: `primary_index_` is -1 exactly
// when the list is empty, and otherwise a valid index into it. Order is
// INSERTION order, never the tree's visual order — `extend_range` is handed
// the visual ordering to compute a span but still only appends.
//
// Copyable and cheap; holds no engine resources. Not thread-safe and not meant
// to be: it is app-thread UI state.
class SelectionSet {
public:
    // Replace the entire selection with a single object.
    void replace(const SelectedObject& obj);

    // Toggle an object in/out of the selection (Ctrl+click).
    // Adding makes it primary. Removing the primary promotes the LAST
    // remaining item, not the neighbour of the removed one.
    void toggle(const SelectedObject& obj);

    // Extend selection to include a range (Shift+click).
    // `ordered_ids` is the current visual ordering of all objects in the tree.
    // Selects everything between the primary and the target.
    // Additive only — nothing already selected is dropped, and the primary
    // stays the same object so repeated shift+clicks re-anchor from one end.
    // Falls back to `replace(target)` when there is no primary or either
    // endpoint is missing from `ordered_ids`.
    void extend_range(const SelectedObject& target,
                      const std::vector<SelectedObject>& ordered_ids);

    // Clear the entire selection.
    void clear();

    // Atomically install one already-validated ordered selection.  `primary`
    // is an index into `items`, or -1 only when `items` is empty.  This is the
    // bulk counterpart to the click gestures above: it lets an app-thread
    // command validate its whole request before making any visible change.
    // Returns false for a malformed input and otherwise reports whether the
    // ordered selection or primary changed.
    bool assign(std::vector<SelectedObject> items, int primary);

    // The primary (most recently clicked) object. Returns nullptr if empty.
    const SelectedObject* primary() const;

    // All selected objects.
    const std::vector<SelectedObject>& items() const { return items_; }

    // Is a specific object selected?
    // Linear scan; the selection is expected to be small.
    bool contains(const SelectedObject& obj) const;

    // Number of selected objects.
    size_t size() const { return items_.size(); }
    bool empty() const { return items_.empty(); }

    // Monotonic app-session revision. It advances only when the ordered set or
    // primary actually changes; idempotent clears/replacements do not make an
    // optimistic-concurrency token stale.
    uint64_t revision() const { return revision_; }

    // Remove objects that no longer exist. Call once per frame.
    // `alive` returns true if the object still exists in the scene.
    // Called once per selected object, so keep that lookup cheap. A surviving
    // primary stays primary; a pruned one promotes the last survivor rather
    // than clearing the selection.
    void validate(std::function<bool(const SelectedObject&)> alive);

private:
    std::vector<SelectedObject> items_;
    int primary_index_ = -1;  // index into items_ of the primary selection
    uint64_t revision_ = 0;
};

} // namespace viewer
