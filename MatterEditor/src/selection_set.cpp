// MatterEditor/src/selection_set.cpp
//
// The three click gestures and the per-frame prune, over an ordered vector
// plus a primary index. See selection_set.h for the type and the invariant.
//
// The single rule every function here maintains: `primary_index_` is either -1
// with an empty list, or a valid index into `items_`. Each function below
// re-establishes it explicitly rather than relying on the previous state,
// because the list is mutated by clicks, by range extension and by the
// liveness prune, all of which can move or delete the primary.
//
// Ordering is INSERTION order, not the tree's visual order. `extend_range` is
// the only function handed the visual ordering, and even it only appends —
// nothing here ever reorders `items_` to match the tree.
//
// No locking and no callbacks: this is plain app-thread data that main.cpp
// owns, the panels write, and the outline/gizmo/pick code reads.

#include "selection_set.h"

#include <algorithm>

namespace viewer {

void SelectionSet::replace(const SelectedObject& obj) {
    if (items_.size() == 1 && items_[0] == obj && primary_index_ == 0) return;
    items_.clear();
    items_.push_back(obj);
    primary_index_ = 0;
    ++revision_;
}

// Ctrl+click. Adding makes the new object primary. Removing repairs the index:
// if the removed object WAS the primary, the primary becomes the last
// remaining item (not the neighbour of the removed one), and if it sat before
// the primary the index shifts down to keep pointing at the same object.
void SelectionSet::toggle(const SelectedObject& obj) {
    ++revision_;
    auto it = std::find(items_.begin(), items_.end(), obj);
    if (it != items_.end()) {
        int removed_index = static_cast<int>(it - items_.begin());
        items_.erase(it);
        if (items_.empty()) {
            primary_index_ = -1;
        } else if (removed_index == primary_index_) {
            primary_index_ = static_cast<int>(items_.size()) - 1;
        } else if (removed_index < primary_index_) {
            --primary_index_;
        }
        return;
    }
    items_.push_back(obj);
    primary_index_ = static_cast<int>(items_.size()) - 1;
}

// Shift+click. `ordered_ids` must be the tree's CURRENT visual ordering — the
// range is the span between the primary and `target` within that list, so a
// stale or differently-ordered list selects the wrong span rather than
// failing.
//
// Purely additive: objects already selected outside the span are kept, and the
// primary deliberately stays the same object (this is what lets repeated
// shift+clicks grow and re-anchor from one fixed end). Degrades to a plain
// `replace(target)` when there is no valid primary, or when either endpoint is
// absent from `ordered_ids`.
void SelectionSet::extend_range(const SelectedObject& target,
                                 const std::vector<SelectedObject>& ordered_ids) {
    if (primary_index_ < 0 || primary_index_ >= static_cast<int>(items_.size())) {
        replace(target);
        return;
    }
    const SelectedObject primary_obj = items_[primary_index_];

    auto primary_it = std::find(ordered_ids.begin(), ordered_ids.end(), primary_obj);
    auto target_it = std::find(ordered_ids.begin(), ordered_ids.end(), target);
    if (primary_it == ordered_ids.end() || target_it == ordered_ids.end()) {
        replace(target);
        return;
    }

    size_t start = static_cast<size_t>(primary_it - ordered_ids.begin());
    size_t end = static_cast<size_t>(target_it - ordered_ids.begin());
    if (start > end) std::swap(start, end);

    bool changed = false;
    for (size_t i = start; i <= end; ++i) {
        if (!contains(ordered_ids[i])) {
            items_.push_back(ordered_ids[i]);
            changed = true;
        }
    }

    // primary_index_ still refers to the same object since we only appended.
    auto new_primary_it = std::find(items_.begin(), items_.end(), primary_obj);
    primary_index_ = static_cast<int>(new_primary_it - items_.begin());
    if (changed) ++revision_;
}

void SelectionSet::clear() {
    if (items_.empty() && primary_index_ == -1) return;
    items_.clear();
    primary_index_ = -1;
    ++revision_;
}

const SelectedObject* SelectionSet::primary() const {
    if (primary_index_ < 0 || primary_index_ >= static_cast<int>(items_.size())) {
        return nullptr;
    }
    return &items_[primary_index_];
}

bool SelectionSet::contains(const SelectedObject& obj) const {
    return std::find(items_.begin(), items_.end(), obj) != items_.end();
}

// Drop everything `alive` rejects. Invokes the callback once per selected
// object, so it is the caller's job to keep that lookup cheap. If the primary
// survived it stays primary; if it did not, the primary becomes the last
// surviving item rather than the selection being cleared. An empty result
// resets the index to -1.
void SelectionSet::validate(std::function<bool(const SelectedObject&)> alive) {
    const size_t previous_size = items_.size();
    const SelectedObject* primary_obj = primary();
    SelectedObject saved_primary{};
    bool had_primary = primary_obj != nullptr;
    if (had_primary) saved_primary = *primary_obj;

    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                 [&](const SelectedObject& o) { return !alive(o); }),
                 items_.end());

    if (items_.empty()) {
        primary_index_ = -1;
        if (previous_size != 0) ++revision_;
        return;
    }

    if (had_primary) {
        auto it = std::find(items_.begin(), items_.end(), saved_primary);
        if (it != items_.end()) {
            primary_index_ = static_cast<int>(it - items_.begin());
            if (items_.size() != previous_size) ++revision_;
            return;
        }
    }
    primary_index_ = static_cast<int>(items_.size()) - 1;
    if (items_.size() != previous_size) ++revision_;
}

} // namespace viewer
