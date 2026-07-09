#include "particle_flow.h"

namespace pf {

PathRecorder::PathRecorder(float min_segment, const std::vector<std::string>& names)
    : min_seg_(min_segment > 0 ? min_segment : 0.0f) {
    set_.channel_names = names;
}

void PathRecorder::append_vertex(const Sim& s, uint32_t slot, uint32_t path_index) {
    const float* pd = s.pos_data();
    PathSet::Path& path = set_.paths[path_index];
    path.xyz.push_back(pd[3*slot]);
    path.xyz.push_back(pd[3*slot+1]);
    path.xyz.push_back(pd[3*slot+2]);
    for (uint32_t c = 0; c < s.channel_count(); ++c)
        path.channels[c].push_back(s.attr_data(c)[slot]);
}

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
