// libs/ParticleFlowLib/src/pf_path_recorder.cpp
//
// `PathRecorder`: the standard `ITickObserver`, and the thing world scripts
// actually consume — `MatterEngine3/src/pf_bindings.cpp` hands its `PathSet`
// back to JS, where `Tree.js` / `TreeBranch.js` turn the polylines into
// geometry.
//
// Invoked from `Sim::step` after every particle has integrated, so it always
// sees end-of-tick positions. Per tick it does three things, in this order:
// open a path for each id it has not seen before (recording the spawn vertex),
// append a vertex for each moving particle that has travelled at least
// `min_segment` since its last recorded vertex, then close the paths of
// everything in `died_this_tick()` after recording a final vertex.
//
// Append-only, structurally: paths are only pushed, vertices are only pushed,
// nothing already written is ever revised. That is what makes a partially-run
// sim's output usable.
//
// Memory: the id->track table is indexed directly by particle id, which only
// increases, so the table grows with the number of particles EVER emitted, not
// with the live population. A long run that churns short-lived particles pays
// for all of them.
#include "particle_flow.h"

namespace pf {

// `min_segment` is the decimation distance in the caller's units; a negative
// value is clamped to 0, and 0 disables intermediate vertices entirely (paths
// then hold only the spawn vertex and, if the particle moved, a death vertex).
// `names` is stored verbatim as `PathSet::channel_names` and is never checked
// against the Sim's actual channel count — see the note on that field.
PathRecorder::PathRecorder(float min_segment, const std::vector<std::string>& names)
    : min_seg_(min_segment > 0 ? min_segment : 0.0f) {
    set_.channel_names = names;
}

// Append the slot's current position plus one sample of every ATTRIBUTE channel
// (state channels are deliberately not recorded). Assumes the path's `channels`
// vector was already sized to `s.channel_count()` when the path was opened, so
// this must not be called for a path opened against a differently-shaped Sim.
void PathRecorder::append_vertex(const Sim& s, uint32_t slot, uint32_t path_index) {
    const float* pd = s.pos_data();
    PathSet::Path& path = set_.paths[path_index];
    path.xyz.push_back(pd[3*slot]);
    path.xyz.push_back(pd[3*slot+1]);
    path.xyz.push_back(pd[3*slot+2]);
    for (uint32_t c = 0; c < s.channel_count(); ++c)
        path.channels[c].push_back(s.attr_data(c)[slot]);
}

// Ascending slot order keeps path creation order reproducible for a given seed.
// The tick number is unused — decimation is by distance travelled, not by time.
//
// Deaths are handled after the movement pass and read the dead slots' positions
// directly: `Sim::kill_slot` leaves position and velocity intact, and a slot
// freed this tick cannot have been reused yet (emitters run before integration),
// so the value read is genuinely the death position. The `path.closed` guard
// makes a repeated death report harmless.
void PathRecorder::on_tick(const Sim& s, uint32_t) {
    const float* pd = s.pos_data();
    const uint8_t* alive = s.alive_data();
    // Ascending slot order = deterministic. New ids start paths; movement
    // appends decimated vertices.
    for (uint32_t slot = 0; slot < s.slot_count(); ++slot) {
        if (!alive[slot]) continue;
        uint32_t id = s.id_of(slot);
        if (id >= known_.size()) { known_.resize(id + 1, 0); by_id_.resize(id + 1); }
        V3 p{pd[3*slot], pd[3*slot+1], pd[3*slot+2]};
        if (!known_[id]) {
            known_[id] = 1;
            PathSet::Path path;
            path.particle_id = id;
            path.channels.resize(s.channel_count());
            set_.paths.push_back(std::move(path));
            by_id_[id] = {(uint32_t)set_.paths.size() - 1, p};
            append_vertex(s, slot, by_id_[id].path_index);
            continue;
        }
        Track& t = by_id_[id];
        if (length(p - t.last) >= min_seg_ && min_seg_ > 0) {
            append_vertex(s, slot, t.path_index);
            t.last = p;
        }
    }
    // Deaths this tick: record the final position and close the path.
    for (uint32_t slot : s.died_this_tick()) {
        uint32_t id = s.id_of(slot);
        if (id >= known_.size() || !known_[id]) continue;
        Track& t = by_id_[id];
        PathSet::Path& path = set_.paths[t.path_index];
        if (path.closed) continue;
        V3 p{pd[3*slot], pd[3*slot+1], pd[3*slot+2]};
        if (length(p - t.last) > 1e-6f) append_vertex(s, slot, t.path_index);
        path.closed = true;
    }
}

V3 path_end_dir(const PathSet::Path& p) {
    size_t n = p.vertex_count();
    if (n < 2) return {0, 0, 0};
    V3 a{p.xyz[3*(n-2)], p.xyz[3*(n-2)+1], p.xyz[3*(n-2)+2]};
    V3 b{p.xyz[3*(n-1)], p.xyz[3*(n-1)+1], p.xyz[3*(n-1)+2]};
    return normalize(b - a);
}

} // namespace pf
