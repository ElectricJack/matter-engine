#pragma once

// MatterEditor/src/scene_diff.h
//
// The join behind the agent protocol's `scene.capture_snapshot`,
// `scene.list_snapshots`, `scene.diff` and `scene.query`: what actually
// changed between two generations of a procedural world, and which objects
// are where.
//
// WHY THIS IS NOT "diff the two object lists"
//
// A baked root's id IS its resolved content hash (scene_inventory.h). Rebake
// the part and the id changes, so a diff keyed on the id reports every root as
// one removal plus one addition and never says the useful thing: the SAME
// logical thing was regenerated, and here is what its new incarnation differs
// in. So every object carries two identities here:
//
//   * the INCARNATION -- `agent::ObjectIdentity`, exactly the {kind,id} the
//     rest of the protocol takes, valid only at the revision it was read at;
//   * the LOGICAL KEY -- what survives a regeneration: the module name for a
//     baked root, the authored id for an authored entity.
//
// Pairing is done on the logical key; the incarnation is then a FIELD that can
// change, and `regenerated` says so in as many words.
//
// WHAT IS NOT PAIRED, AND SAYS SO
//
// Two populations have no logical key that survives:
//
//   * an entity whose id was minted at runtime (`matter::scene::is_runtime_id`)
//     is session-scoped. Across two snapshots from different sessions the same
//     number is a different object, so those objects are reported as
//     `incomparable` with the reason, never paired;
//   * a module published as MORE THAN ONE baked root has an ambiguous logical
//     key. Two roots and two roots is four possible pairings and nothing in
//     the part graph picks one, so all of them are reported `incomparable`.
//
// And two whole snapshots can be incomparable: different worlds or different
// projects share nothing but the shape of their ids. `compatibility_of` says
// `incomparable` and the diff carries no rows, because a list of "everything
// was removed and everything was added" would read like a finding.
//
// SPATIAL QUERIES
//
// `scene.query`'s region filter runs over the world AABB captured for each
// object, which is derived (`inventory::world_aabb_from_local`, all eight
// transformed corners) from the SAME local box + world matrix the selection
// outline draws and the viewport pick tests -- `viewer::bounds_for_object_set`
// in selection_bounds.h. There is deliberately no second acceleration
// structure: the capture already costs one ECS scan, the editor names
// thousands of objects rather than millions, and a BVH built per query would
// be slower than the linear box test it replaced while introducing a second
// definition of "where is this object", which is the drift this file exists to
// prevent.
//
// An object whose bounds did not resolve (a root in the part graph that is
// placed nowhere in this world; an entity with no transform in the live ECS)
// is NOT silently excluded from a region query: it is counted and reported as
// `unresolved`, because "not in the region" and "we could not tell" are
// different answers.
//
// Deliberately engine-free, like scene_inventory.h: main.cpp measures the live
// sources and hands plain structs in, and every rule that could be wrong --
// pairing, comparability, field comparison, ordering, paging, region tests --
// is exercised with no session, no renderer and no ImGui by
// MatterEditor/tests/test_scene_diff.cpp.

#include "agent_protocol.h"
#include "scene_inventory.h"
#include "matter/json_doc.h"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace viewer::scenediff {

// --- bounds ------------------------------------------------------------------

// Objects captured per snapshot. A snapshot is retained in editor memory, so
// this is a memory bound as much as a result bound: the capture is ordered
// (kind, id) BEFORE the cap applies, so a truncated capture is a deterministic
// prefix rather than an arbitrary subset, and `Snapshot::truncated` says the
// tail is missing instead of letting it read as a deletion.
constexpr std::uint32_t kMaxCapturedObjects = 20000;

// Retained snapshots. Oldest is evicted; `SnapshotStore::oldest_retained_id`
// separates "aged out" from "never existed", exactly as the job ledger does.
constexpr std::size_t kRetainedSnapshots = 8;

// Page bounds, matching scene.list_objects: a row here is comparable in size.
constexpr std::uint32_t kDefaultPageLimit = 100;
constexpr std::uint32_t kMaxPageLimit = 200;

// Per-object text and list caps applied at capture, so one pathological name
// or component list cannot make a retained snapshot unbounded.
constexpr std::size_t kMaxCapturedTextBytes = 256;
constexpr std::size_t kMaxCapturedComponents = 32;

// Incomparable rows and ambiguous keys are diagnostics, not the payload.
constexpr std::size_t kMaxReportedIncomparable = 32;
constexpr std::size_t kMaxReportedAmbiguousKeys = 16;

// The world-space AABB measured for one object at capture time, or the reason
// there is not one. No default-fabricated box: an unresolved object is a
// normal outcome and a zero box is a real box.
struct MeasuredBounds {
    bool resolved = false;
    std::string reason;
    float world_min[3] = {0.0f, 0.0f, 0.0f};
    float world_max[3] = {0.0f, 0.0f, 0.0f};
};

// --- logical identity --------------------------------------------------------

// What survives a regeneration. `stability` is the same vocabulary the
// identity contract emits per object (scene_inventory.h), so a caller reading
// a diff row and a listing row sees one story.
struct LogicalKey {
    enum class Stability {
        WorldDefinition,  // authored entity id hash: stable across reloads
        Module,           // baked-root module name: stable across rebakes
        Session,          // runtime-minted entity id: this session only
    };
    agent::ObjectIdentity::Kind kind = agent::ObjectIdentity::Kind::Entity;
    Stability stability = Stability::WorldDefinition;
    std::uint64_t numeric = 0;  // entity id; 0 for a module key
    std::string text;           // module name for a baked root, else empty
};

LogicalKey logical_key_for(const inventory::Entry& entry);
bool same_logical_key(const LogicalKey& a, const LogicalKey& b);
// Total order: (kind, stability, numeric, text). Total so that paging a diff
// at a fixed pair of snapshots is resumable.
bool logical_key_less(const LogicalKey& a, const LogicalKey& b);
const char* stability_name(LogicalKey::Stability stability);

// --- a captured object -------------------------------------------------------

// The comparable half of an `inventory::Entry` plus its measured bounds.
// Canonical parameters are kept as a DIGEST, not as text: a diff needs to know
// that the generation inputs changed, and retaining every root's parameter
// JSON in every snapshot is how a snapshot ring turns into a memory leak. The
// digest is `inventory::fnv1a64`, the same one `scene.trace_provenance`
// publishes as `params_hash`, so the two can be compared directly.
struct CapturedObject {
    agent::ObjectIdentity object;
    LogicalKey key;
    // True when this object's logical key does not identify it uniquely within
    // its own snapshot (a module published as several roots). Such objects are
    // never paired.
    bool key_ambiguous = false;
    std::string key_ambiguity_reason;

    inventory::Availability name;
    std::string name_value;
    inventory::Availability path;
    std::string path_value;

    bool has_parent = false;
    agent::ObjectIdentity parent;
    std::uint32_t depth = 0;
    std::uint32_t child_count = 0;
    std::vector<std::string> component_names;
    bool components_truncated = false;

    inventory::Availability provenance;
    std::string module;
    inventory::Availability source_path;
    std::string source_path_value;
    inventory::Availability params;
    std::uint64_t params_digest = 0;
    inventory::Availability world_seed;
    std::uint64_t world_seed_value = 0;

    inventory::Availability part_instance;
    std::uint64_t part_hash = 0;

    MeasuredBounds bounds;
};

// --- a snapshot --------------------------------------------------------------

// The generation inputs the EDITOR can actually attest to. Every one is an
// availability: a world seed is reported only when a completed seeded
// regeneration produced the very scene generation being captured, because a
// seed from some earlier bake would name an input this scene was not built
// from.
struct GenerationInputs {
    inventory::Availability world;
    std::string world_value;
    inventory::Availability project;
    std::string project_value;
    inventory::Availability world_seed;
    std::uint64_t world_seed_value = 0;
    std::string world_seed_source;  // e.g. "regeneration job 4"
    // FNV-1a over the SORTED published (module, resolved hash) roots -- the
    // same digest `job.status` reports, so "the same seed produced the same
    // world" is one number in both places.
    inventory::Availability content_digest;
    std::uint64_t content_digest_value = 0;
};

struct CaptureContext {
    bool session_open = false;
    bool scene_ready = false;
    std::uint64_t session_id = 0;
    std::uint64_t session_generation = 0;
    std::uint64_t scene_generation = 0;
    std::uint64_t scene_revision = 0;
};

struct Snapshot {
    std::uint64_t snapshot_id = 0;  // 0 until a SnapshotStore retains it
    std::string label;
    CaptureContext context;
    GenerationInputs inputs;

    std::vector<CapturedObject> objects;  // ordered (kind, id)
    std::uint64_t entity_count = 0;       // whole scene, before the cap
    std::uint64_t baked_root_count = 0;   // whole scene, before the cap
    std::uint64_t named_total = 0;        // entity_count + baked_root_count
    bool truncated = false;
    std::uint32_t capture_limit = 0;

    std::uint64_t bounds_resolved = 0;
    std::uint64_t bounds_unresolved = 0;

    std::vector<std::string> ambiguous_keys;  // bounded sample
    std::uint64_t ambiguous_key_count = 0;
    std::uint64_t ambiguous_object_count = 0;
};

// `bounds` is parallel to `inventory_snapshot.entries` and must be the same
// length; an empty vector means "no bounds were measured at all" and every
// object records that reason rather than a missing box.
Snapshot capture(const inventory::Snapshot& inventory_snapshot,
                 const std::vector<MeasuredBounds>& bounds,
                 const CaptureContext& context, const GenerationInputs& inputs,
                 std::string label,
                 std::uint32_t limit = kMaxCapturedObjects);

// The retained ring. App thread only, like the job ledger.
class SnapshotStore {
public:
    explicit SnapshotStore(std::size_t capacity = kRetainedSnapshots);

    // Assigns the next id (ids start at 1, increase monotonically, are never
    // reused) and returns it. Evicting the oldest is recorded, not silent.
    std::uint64_t retain(Snapshot snapshot);
    const Snapshot* find(std::uint64_t id) const;
    const std::deque<Snapshot>& retained() const { return snapshots_; }
    std::size_t capacity() const { return capacity_; }
    std::uint64_t total_captured() const { return next_id_ - 1; }
    std::uint64_t oldest_retained_id() const;  // 0 when nothing is retained
    std::uint64_t last_evicted_id() const { return last_evicted_; }

private:
    std::size_t capacity_;
    std::deque<Snapshot> snapshots_;
    std::uint64_t next_id_ = 1;
    std::uint64_t last_evicted_ = 0;
};

// --- comparability -----------------------------------------------------------

enum class Comparability { Full, Partial, Incomparable };
const char* comparability_name(Comparability level);

struct IncomparableClass {
    std::string name;
    std::string reason;
};

struct Compatibility {
    Comparability level = Comparability::Full;
    std::string reason;  // set for Partial and Incomparable
    std::vector<IncomparableClass> classes;
    // False when the two snapshots come from different sessions/generations,
    // which is exactly when a runtime-minted entity id means different things
    // on the two sides.
    bool session_scoped_ids_comparable = true;
};

Compatibility compatibility_of(const Snapshot& from, const Snapshot& to);

// --- the diff ----------------------------------------------------------------

enum class ChangeKind { Added, Removed, Changed, Unchanged, Incomparable };
const char* change_name(ChangeKind change);

// One differing field, printable. `before` / `after` are JSON values so a row
// carries the actual old and new value rather than a sentence about it.
struct FieldChange {
    std::string field;
    matter::jsondoc::Value before;
    matter::jsondoc::Value after;
};

struct DiffRow {
    ChangeKind change = ChangeKind::Unchanged;
    LogicalKey key;
    bool has_before = false;
    bool has_after = false;
    std::size_t before_index = 0;  // into Snapshot::objects of `from`
    std::size_t after_index = 0;   // into Snapshot::objects of `to`
    // A paired object whose incarnation id changed: the SAME logical thing was
    // rebaked. Always accompanied by an `incarnation` field change.
    bool regenerated = false;
    std::string reason;  // Incomparable rows only
    std::vector<FieldChange> changes;
};

struct Diff {
    Compatibility compatibility;
    std::vector<DiffRow> rows;  // Added/Removed/Changed/Unchanged, ordered
    std::vector<DiffRow> incomparable;  // never paired; bounded when serialized
    std::uint64_t added = 0;
    std::uint64_t removed = 0;
    std::uint64_t changed = 0;
    std::uint64_t unchanged = 0;
    std::uint64_t regenerated = 0;
    std::uint64_t incomparable_count = 0;
    // True only when BOTH snapshots published a content digest and the two are
    // equal. Unavailable digests never read as "identical".
    bool content_identical = false;
    bool content_comparable = false;
};

Diff compare(const Snapshot& from, const Snapshot& to);

struct DiffQuery {
    std::uint64_t from_id = 0;
    bool to_is_current = true;  // "current" captures the live scene at dispatch
    std::uint64_t to_id = 0;
    bool include_entities = true;
    bool include_baked_roots = true;
    // Unchanged rows are excluded by default: on a large world they are the
    // whole scene and they are exactly the rows a caller asked the diff to
    // filter out for them.
    bool include_added = true;
    bool include_removed = true;
    bool include_changed = true;
    bool include_unchanged = false;
    std::uint64_t offset = 0;
    std::uint32_t limit = kDefaultPageLimit;
};

bool parse_diff_query(const matter::jsondoc::Value& arguments, DiffQuery& out,
                      std::string& error);

struct DiffPage {
    std::vector<const DiffRow*> rows;  // borrowed from the Diff
    std::uint64_t total_matched = 0;
    std::uint64_t offset = 0;
    std::uint32_t limit = 0;
    bool has_more = false;
    std::uint64_t next_offset = 0;  // meaningful only when has_more
};

DiffPage page_diff(const Diff& diff, const DiffQuery& query);

matter::jsondoc::Value diff_result_json(const Snapshot& from, const Snapshot& to,
                                        const Diff& diff, const DiffQuery& query,
                                        const DiffPage& page);

// --- bounded object queries ---------------------------------------------------

struct Region {
    enum class Type { None, Aabb, Sphere } type = Type::None;
    // `Intersects` keeps an object whose world box touches the region;
    // `Contains` keeps only one wholly inside it.
    enum class Mode { Intersects, Contains } mode = Mode::Intersects;
    float min[3] = {0.0f, 0.0f, 0.0f};
    float max[3] = {0.0f, 0.0f, 0.0f};
    float center[3] = {0.0f, 0.0f, 0.0f};
    float radius = 0.0f;
};

// True when the object's world box satisfies `region`. `region.type == None`
// is not a filter and this is never called for it.
bool region_matches(const Region& region, const float world_min[3],
                    const float world_max[3]);

struct ObjectQuery {
    bool snapshot_is_current = true;
    std::uint64_t snapshot_id = 0;
    bool include_entities = true;
    bool include_baked_roots = true;
    std::string name_contains;         // case-insensitive
    std::string module_contains;       // case-insensitive, provenance module
    std::string source_path_contains;  // case-insensitive, recorded source path
    bool filter_has_provenance = false;
    bool has_provenance = false;
    bool filter_has_part_instance = false;
    bool has_part_instance = false;
    Region region;
    std::uint64_t offset = 0;
    std::uint32_t limit = kDefaultPageLimit;
};

bool parse_object_query(const matter::jsondoc::Value& arguments, ObjectQuery& out,
                        std::string& error);

struct QueryPage {
    std::vector<const CapturedObject*> objects;  // borrowed from the Snapshot
    std::uint64_t total_matched = 0;
    std::uint64_t offset = 0;
    std::uint32_t limit = 0;
    bool has_more = false;
    std::uint64_t next_offset = 0;
    // Region accounting, reported whenever a region was supplied. `unresolved`
    // is the count that could not be tested at all -- never folded into
    // "rejected", which would report a measurement the editor did not make.
    std::uint64_t region_tested = 0;
    std::uint64_t region_matched = 0;
    std::uint64_t region_unresolved = 0;
};

QueryPage query_objects(const Snapshot& snapshot, const ObjectQuery& query);

// --- serialization -------------------------------------------------------------

matter::jsondoc::Value snapshot_record_json(const Snapshot& snapshot);
matter::jsondoc::Value capture_result_json(const Snapshot& snapshot,
                                           const SnapshotStore& store);
matter::jsondoc::Value snapshot_list_json(const SnapshotStore& store);
matter::jsondoc::Value query_result_json(const Snapshot& snapshot,
                                         const ObjectQuery& query,
                                         const QueryPage& page);
// The `found:false` answer for a snapshot id, carrying what is retained so
// "aged out of the ring" stays distinguishable from "never existed".
matter::jsondoc::Value missing_snapshot_json(std::uint64_t snapshot_id,
                                             const SnapshotStore& store,
                                             const std::string& reason);

}  // namespace viewer::scenediff
