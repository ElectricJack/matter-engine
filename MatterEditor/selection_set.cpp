#include "selection_set.h"

#include <algorithm>

namespace viewer {

void SelectionSet::replace(const SelectedObject& obj) {
    items_.clear();
    items_.push_back(obj);
    primary_index_ = 0;
}

void SelectionSet::toggle(const SelectedObject& obj) {
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

    for (size_t i = start; i <= end; ++i) {
        if (!contains(ordered_ids[i])) {
            items_.push_back(ordered_ids[i]);
        }
    }

    // primary_index_ still refers to the same object since we only appended.
    auto new_primary_it = std::find(items_.begin(), items_.end(), primary_obj);
    primary_index_ = static_cast<int>(new_primary_it - items_.begin());
}

void SelectionSet::clear() {
    items_.clear();
    primary_index_ = -1;
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

void SelectionSet::validate(std::function<bool(const SelectedObject&)> alive) {
    const SelectedObject* primary_obj = primary();
    SelectedObject saved_primary{};
    bool had_primary = primary_obj != nullptr;
    if (had_primary) saved_primary = *primary_obj;

    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                 [&](const SelectedObject& o) { return !alive(o); }),
                 items_.end());

    if (items_.empty()) {
        primary_index_ = -1;
        return;
    }

    if (had_primary) {
        auto it = std::find(items_.begin(), items_.end(), saved_primary);
        if (it != items_.end()) {
            primary_index_ = static_cast<int>(it - items_.begin());
            return;
        }
    }
    primary_index_ = static_cast<int>(items_.size()) - 1;
}

} // namespace viewer
