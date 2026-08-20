#pragma once

// MatterEditor/src/editor_model.h
//
// The editor-side model behind the Scene/outliner panel: an authoritative map
// of SceneRecords, the flattened preorder hierarchy derived from it, the
// current selection, and thin wrappers over the engine's scene-mutation
// commands.
//
// It is engine-agnostic in both directions on purpose. Inbound, rows arrive as
// canonical scene.rows_upserted / scene.rows_removed deltas fed by
// scene_model_adapter.* (see the E5c note on SceneCommands below); outbound,
// mutations go through the SceneCommands closures rather than through the ECS
// world. That is what lets MatterEngine3/tests/editor_model_tests.cpp drive
// the whole model with no session and no renderer. Nothing ImGui is included
// here — only matter/scene.h and matter/event/property.h.
//
// Lifetime and threading: main.cpp owns one EditorModel for the process and
// calls attach_scheduler() once, after the app's PropertyScheduler exists.
// Nothing here locks — every method is called from the editor's main loop, and
// the coalesced re-flatten runs inside the scheduler's flush on that same
// thread.
//
// Ids: a SceneEntityId `value` of 0 is the universal sentinel — "no parent" on
// a record, "nothing selected" on a Selection.

#include "matter/scene.h"
#include "matter/event/property.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace viewer {

// What the outliner and the viewport agree is selected — they share one
// instance rather than each keeping their own.
struct Selection {
    matter::scene::SceneEntityId id{};
    // World generation at the moment of selection, stamped by select() from
    // last_generation_. Only the legacy poll path (refresh) ever advances that
    // counter, so on the delta-driven viewer this stays 0; no code currently
    // reads it back.
    uint64_t world_generation = 0;
};

// A flattened row in the hierarchy tree (preorder traversal).
struct HierarchyRow {
    matter::scene::SceneEntityId id{};
    matter::scene::SceneEntityId parent_id{};
    std::string name;
    // Preorder depth: 0 for a root, +1 per level. The rows are stored flat, so
    // this is the only indentation cue the panel gets.
    uint32_t depth = 0;
    // Direct children in the record STORE, not in the filtered row list — a
    // row can report children that the active filter is hiding.
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

// The outliner's model: an authoritative SceneRecord store plus the flattened,
// filtered hierarchy derived from it.
//
// Call order: attach_scheduler() once, then feed rows — apply_snapshot() for a
// full set, apply_upsert()/apply_remove() for deltas. rows() reflects the last
// completed re-flatten, which for the delta paths happens at the next
// PropertyScheduler flush rather than inside the delta call, so a panel that
// draws between the delta and the flush sees the previous row set for one
// frame.
//
// Holds a Subscription and a Property that reference the app scheduler, so it
// must not outlive it and is not meant to be copied. No locks: main-loop only,
// including the observer installed by attach_scheduler().
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
    // In practice the match is case-insensitive and against the row NAME only
    // — the id is not searched. Reapplies the filter immediately; it does not
    // wait for a flush.
    void set_filter(const std::string& filter);
    const std::string& filter() const { return filter_; }

    // Selection (viewport and outliner share the same selection).
    void select(matter::scene::SceneEntityId id);
    void clear_selection();
    const Selection& selection() const { return selection_; }
    bool has_selection() const { return selection_.id.value != 0; }

    // Hierarchy access. Both report the FILTERED rows, so row_count() shrinks
    // as you type into the filter box and the unfiltered set is not exposed.
    // The returned reference is invalidated by the next re-flatten (any
    // apply_snapshot, or any delta once the scheduler flushes), so do not hold
    // it across a frame.
    const std::vector<HierarchyRow>& rows() const { return filtered_rows_; }
    uint32_t row_count() const { return static_cast<uint32_t>(filtered_rows_.size()); }

    // Commands (return error on failure, None on success).
    //
    // `commands` is borrowed per call, never stored. Each command moves the
    // selection as a side effect: create_empty/duplicate_selected select what
    // they created, delete_selected clears it, reparent_selected leaves it
    // alone. A command whose closure is null returns InvalidTarget; one that
    // needs a selection and has none returns EntityNotFound. None of them
    // update the row set — that happens when the resulting delta arrives.
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
