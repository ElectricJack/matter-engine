#pragma once

// MatterEditor/src/scene_inventory.h
//
// The join behind the agent protocol's `scene.list_objects` and
// `scene.get_object`: bounded enumeration of everything the editor can name in
// the current world, and the exact inspection of one of those objects.
//
// The editor has TWO object populations and they are not one list:
//
//   * authored entities — SceneEntityId-bearing ECS rows the EditorModel keeps
//     (created by SceneService, reparentable, deletable);
//   * baked roots — the root nodes of the session's part-graph snapshot,
//     addressed by their resolved CONTENT hash, which are bake outputs and not
//     authored objects at all.
//
// They share this file and nothing else. Their ids live in separate namespaces
// and the same 64-bit number is a perfectly ordinary collision: entity 42 and
// baked_root 42 are different objects. That is why every id here travels as an
// `agent::ObjectIdentity` (kind + id), why ordering sorts on (kind, id) rather
// than on id, and why an id is serialized as a decimal STRING — a JSON number
// silently loses part hashes above 2^53.
//
// Deliberately engine-free, exactly like editor_model.h: main.cpp adapts the
// live sources (EditorModel rows, WorldSession::graph_snapshot, SelectionSet,
// viewer::bounds_for_objects) into the plain structs below, and every rule that
// could be wrong — ordering, paging, filtering, id namespacing, availability
// reporting, the world-AABB derivation — is exercised with no session, no
// renderer and no ImGui by MatterEditor/tests/test_scene_inventory.cpp.
//
// Nothing here fabricates a value it does not have. A fact the editor cannot
// answer is reported as `Availability{available=false, reason}` so a caller can
// tell "this object has no authored source path" apart from "the source path is
// empty" apart from "this build does not record source paths".

#include "agent_protocol.h"
#include "matter/json_doc.h"

#include <cstdint>
#include <string>
#include <vector>

namespace viewer::inventory {

// Paging bounds. The protocol replaces a terminal result over
// agent::kMaxResultBytes (1 MiB) with an `output_too_large` error, so the cap
// has to leave real headroom: a row costs roughly 600 bytes of fixed structure
// and can reach ~1.5 KiB with a deep path and a long component list, which puts
// a full 200-row page at a few hundred KiB even in the bad case.
constexpr std::uint32_t kDefaultListLimit = 100;
constexpr std::uint32_t kMaxListLimit = 200;

// A fact that may legitimately be missing. When `available` is false, `reason`
// says WHY, and the value fields it guards are meaningless.
struct Availability {
    bool available = false;
    std::string reason;
};

// --- raw inputs -------------------------------------------------------------
// main.cpp fills these from the live sources; the tests fill them by hand.

// One authored entity, as the EditorModel's flattened hierarchy knows it.
// `parent_id` of 0 means "root" (the SceneEntityId sentinel).
struct EntityRow {
    std::uint64_t id = 0;
    std::uint64_t parent_id = 0;
    std::string name;
    std::uint32_t depth = 0;
    std::uint32_t child_count = 0;
    std::vector<std::string> component_names;
};

// One baked root, as part_graph_snapshot::Node knows it. `source_path` and
// `params_json` are frequently empty and are reported as unavailable rather
// than as an empty string that could be mistaken for a real path.
struct RootRow {
    std::uint64_t resolved_hash = 0;
    std::string module;
    std::string source_path;
    std::string params_json;
};

// The app-wide selection, flattened. `primary_index` is -1 when nothing is
// selected, matching SelectionSet's own invariant.
struct SelectionInput {
    std::vector<agent::ObjectIdentity> items;
    int primary_index = -1;
};

// --- listed facts -----------------------------------------------------------

// Everything `scene.list_objects` reports for one object. Cheap by
// construction: no ECS scan, no bake query, nothing that costs a frame.
struct Entry {
    agent::ObjectIdentity object;

    Availability name;  // guards `name_value`
    std::string name_value;

    // Slash-joined authored path from the root down to this object. Available
    // only when this object AND every ancestor is named — a path with a hole in
    // it would not round-trip and would not identify anything.
    Availability path;
    std::string path_value;

    bool has_parent = false;
    agent::ObjectIdentity parent;
    std::uint32_t depth = 0;
    std::uint32_t child_count = 0;
    std::vector<std::string> component_names;

    // Part-graph provenance. Baked roots have it; authored entities do not, and
    // say so rather than borrowing the module that produced some part they
    // happen to place.
    Availability provenance;
    std::string module;
    Availability source_path;  // guards `source_path_value`
    std::string source_path_value;
    Availability params;  // guards `params_json`
    std::string params_json;

    bool selected = false;
    bool primary = false;
};

// The whole current population, ordered (kind, then id ascending) at build
// time so every listing, page and re-listing at the same revision agrees.
struct Snapshot {
    std::vector<Entry> entries;
    std::uint64_t scene_revision = 0;
    std::uint64_t entity_count = 0;
    std::uint64_t baked_root_count = 0;
};

Snapshot build_snapshot(const std::vector<EntityRow>& entities,
                        const std::vector<RootRow>& roots,
                        const SelectionInput& selection,
                        std::uint64_t scene_revision);

// --- listing ----------------------------------------------------------------

struct ListQuery {
    bool include_entities = true;
    bool include_baked_roots = true;
    // Case-insensitive substring of the object's NAME (module name for a baked
    // root). Empty matches everything. Ids are never searched: an id filter
    // that matched across kinds would defeat the whole point of the namespaces.
    std::string name_contains;
    std::uint64_t offset = 0;
    std::uint32_t limit = kDefaultListLimit;
};

// Parses and RANGE-CHECKS `scene.list_objects` arguments. The protocol has
// already checked that each declared argument is present and of the declared
// JSON type; this adds the semantics (known kind names, non-empty kind list,
// limit within bounds) and returns a caller-facing `error` for
// Status::InvalidInput.
bool parse_list_query(const matter::jsondoc::Value& arguments, ListQuery& out,
                      std::string& error);

struct ListPage {
    std::vector<const Entry*> entries;  // borrowed from the Snapshot
    std::uint64_t total_matched = 0;
    std::uint64_t offset = 0;
    std::uint32_t limit = 0;
    bool has_more = false;
    std::uint64_t next_offset = 0;  // meaningful only when has_more
};

ListPage list_objects(const Snapshot& snapshot, const ListQuery& query);

// Exact lookup by typed identity. Returns nullptr for an object that is not in
// this snapshot — a deleted entity, a root that a rebake replaced, or a
// numerically valid id that only exists in the OTHER kind's namespace.
const Entry* find_object(const Snapshot& snapshot,
                         const agent::ObjectIdentity& object);

// --- inspection -------------------------------------------------------------

// One thing the editor can do to an object. `available` false always carries a
// reason; `command` names the registered command that performs it, and is
// empty for a capability with no command route (a UI-only affordance).
struct Operation {
    std::string name;
    bool available = false;
    std::string reason;
    std::string command;
};

// `scene.get_object`'s answer: the listed facts plus everything that costs a
// live query. main.cpp fills the live half; `default_operations` fills the
// capability list from the object's kind.
struct Detail {
    Entry entry;

    // Authored visibility flag (PartInstance::visible). Unavailable for an
    // entity that places no part, and for baked roots, whose on-screen presence
    // is decided by LOD and culling rather than by an authored flag.
    Availability visibility;
    bool visible = false;

    // Local-space AABB plus the row-major local->world matrix, exactly as
    // viewer::SelectionBounds carries them. Unavailable for an unloaded root
    // (in the graph, not placed in this world) or an entity the ECS scan did
    // not resolve.
    Availability placement;
    float world_matrix[16] = {};
    float local_min[3] = {};
    float local_max[3] = {};

    std::vector<Operation> operations;
};

// The capability list for `kind`. `has_source_path` gates "open_source"; it is
// ignored for entities, which have no authoring file of their own.
std::vector<Operation> default_operations(agent::ObjectIdentity::Kind kind,
                                          bool has_source_path);

// World-space AABB of the local box under a row-major local->world matrix: the
// min/max over all eight transformed corners, not the transformed min/max
// (which is wrong under rotation). Returns false and leaves the outputs alone
// when the matrix is not finite.
bool world_aabb_from_local(const float local_min[3], const float local_max[3],
                           const float world_matrix[16], float out_min[3],
                           float out_max[3]);

// --- serialization ----------------------------------------------------------

matter::jsondoc::Value list_result_json(const Snapshot& snapshot,
                                        const ListQuery& query,
                                        const ListPage& page);
matter::jsondoc::Value detail_result_json(const Snapshot& snapshot,
                                          const Detail& detail);
// The `found: false` answer, which carries the identity that was asked for and
// the revision the answer is true at, so a caller can tell a deleted object
// apart from a stale question.
matter::jsondoc::Value missing_result_json(const Snapshot& snapshot,
                                           const agent::ObjectIdentity& object,
                                           const std::string& reason);

}  // namespace viewer::inventory
