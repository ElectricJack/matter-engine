#pragma once

#include "matter/scene.h"
#include "matter/event/property.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace viewer {

struct Selection {
    matter::scene::SceneEntityId id{};
    uint64_t world_generation = 0;
};

// A flattened row in the hierarchy tree (preorder traversal).
struct HierarchyRow {
    matter::scene::SceneEntityId id{};
    matter::scene::SceneEntityId parent_id{};
    std::string name;
    uint32_t depth = 0;
    uint32_t child_count = 0;
    std::vector<std::string> component_names;
};

// Callback interface for the engine's scene mutation API.
// The EditorModel uses this instead of directly touching the ECS world.
//
// E5c (event-system.md S I.14): the WINDOWS viewer no longer feeds the model
// through query_records/generation — the model is delta-driven by the
// SessionBinding scene adapter (scene_model_adapter.*) over SceneChangeTracker
// deltas, and the mutation closures issue SceneService commands via the command
// registry (execute()). query_records/generation + EditorModel::refresh() are
// dead in production: main_linux.cpp, the reduced Linux entry point they were
// retained for, is deleted, and the Windows main.cpp leaves them null. The only
// remaining caller is MatterEngine3/tests/editor_model_tests.cpp, which drives
// EditorModel directly (no session) and still needs the poll path.
struct SceneCommands {
    // Query all SceneEntityId-bearing entities as records (legacy poll path;
    // Linux-only). Left null on Windows (delta-driven).
    std::function<std::vector<matter::scene::SceneRecord>()> query_records;
    // Current world generation (legacy poll path; Linux-only). Left null on
    // Windows.
    std::function<uint64_t()> generation;
    // Mutation commands (Windows: each closure issues a SceneService command
    // through the registry; Linux: direct-ECS closures):
    std::function<matter::scene::SceneEditResult(const std::string& name)> create_empty;
    std::function<matter::scene::SceneEditResult(matter::scene::SceneEntityId src)> duplicate;
    std::function<matter::scene::SceneEditResult(matter::scene::SceneEntityId target)> delete_entity;
    std::function<matter::scene::SceneEditResult(matter::scene::SceneEntityId child,
                                                  matter::scene::SceneEntityId new_parent)> reparent;
};

class EditorModel {
public:
    // --- observable delta-driven collection (E5c, event-system.md S I.14) ----
    // Bind the model to the app-owned PropertyScheduler. Incremental deltas
    // (apply_upsert/apply_remove) coalesce their re-flatten through a single
    // observable revision Property delivered at PropertyScheduler::flush_dirty()
    // (once per tick that changed rows), so the panel never re-flattens the
    // whole tree every frame. Call once, before feeding any deltas.
    void attach_scheduler(matter::evt::PropertyScheduler& scheduler);

    // Replace the entire row set (adapter bind / world switch / sequence-gap
    // recovery). Rebuilds the flattened hierarchy SYNCHRONOUSLY (correctness-
    // critical: the tree is correct on the very next draw regardless of flush
    // timing) and revalidates the selection.
    void apply_snapshot(const std::vector<matter::scene::SceneRecord>& rows);

    // Apply a canonical scene.rows_upserted batch: each row is the full current
    // record for an affected id. Merges into the store and marks the hierarchy
    // dirty (re-flatten deferred to the next scheduler flush).
    void apply_upsert(const std::vector<matter::scene::SceneRecord>& rows);

    // Apply a canonical scene.rows_removed batch. Tolerates an unknown id (a
    // same-tick create+delete can remove an id never upserted here) — an erase
    // of a missing key is a silent no-op (E5b review note).
    void apply_remove(const std::vector<matter::scene::SceneEntityId>& ids);

    // Legacy per-frame poll (Linux-only main_linux.cpp). Queries the whole
    // record set through `commands` and applies it as a snapshot. The Windows
    // viewer is delta-driven and never calls this.
    void refresh(const SceneCommands& commands);

    // Filter the hierarchy by name/id substring.
    void set_filter(const std::string& filter);
    const std::string& filter() const { return filter_; }

    // Selection (viewport and outliner share the same selection).
    void select(matter::scene::SceneEntityId id);
    void clear_selection();
    const Selection& selection() const { return selection_; }
    bool has_selection() const { return selection_.id.value != 0; }

    // Hierarchy access.
    const std::vector<HierarchyRow>& rows() const { return filtered_rows_; }
    uint32_t row_count() const { return static_cast<uint32_t>(filtered_rows_.size()); }

    // Commands (return error on failure, None on success).
    matter::scene::SceneEditResult create_empty(const SceneCommands& commands);
    matter::scene::SceneEditResult duplicate_selected(const SceneCommands& commands);
    matter::scene::SceneEditResult delete_selected(const SceneCommands& commands);
    matter::scene::SceneEditResult reparent_selected(const SceneCommands& commands,
                                                      matter::scene::SceneEntityId new_parent);

private:
    // Rebuild the flattened preorder hierarchy from the authoritative record
    // store, reapply the filter, and drop a now-invalid selection.
    void rebuild_hierarchy_from_store();
    void mark_rows_changed();  // dirty + bump the observable revision
    void apply_filter();
    bool is_selection_valid() const;

    // Authoritative record store, keyed by SceneEntityId value. Updated
    // incrementally by the adapter deltas; the flattened rows are derived.
    std::unordered_map<uint64_t, matter::scene::SceneRecord> records_;

    std::vector<HierarchyRow> all_rows_;
    std::vector<HierarchyRow> filtered_rows_;
    Selection selection_{};
    std::string filter_;
    uint64_t last_generation_ = 0;

    // Observable revision: a burst of deltas in one tick coalesces to a single
    // flush delivery (S I.9), whose observer re-flattens when `hierarchy_dirty_`
    // is set. Constructed lazily by attach_scheduler (the scheduler outlives the
    // model but is created after it in main.cpp).
    matter::evt::PropertyScheduler* scheduler_ = nullptr;
    std::unique_ptr<matter::evt::Property<uint64_t>> rows_rev_;
    matter::evt::Subscription rows_sub_;
    uint64_t rev_counter_ = 0;
    bool hierarchy_dirty_ = false;
};

} // namespace viewer
