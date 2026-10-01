#pragma once

#include "vt_surface_connections.h"
#include <iterator>
#include <map>

namespace vt {

// The world authorizes physical part pairs; the renderer supplies the selected
// rung later. Regions are publication units (a sector face or a runtime weld),
// not material identities. Several regions may authorize the same pair.
struct VtSurfacePartPair {
    uint64_t first = 0, second = 0, domain = 0;
};

class VtSurfaceTopology {
public:
    using Region = std::array<uint64_t, 6>;
    using Pair = std::array<uint64_t, 3>; // first, second, explicit domain
    static constexpr size_t kMaxRegionPairs = 512;
    static constexpr size_t kMaxPairs = 65536;
    static constexpr size_t kMaxRegions = 65536;

    bool replace(const Region& region, const std::vector<VtSurfacePartPair>& input,
                 std::string& error) {
        error.clear();
        if (input.size() > kMaxRegionPairs) {
            error = "surface topology region pair budget exceeded"; return false;
        }
        std::vector<Pair> next;
        next.reserve(input.size());
        for (const auto& pair : input) {
            if (!pair.first || !pair.second || !pair.domain || pair.first == pair.second) {
                error = "surface topology requires distinct parts and an explicit domain"; return false;
            }
            next.push_back({std::min(pair.first, pair.second), std::max(pair.first, pair.second), pair.domain});
        }
        std::sort(next.begin(), next.end());
        next.erase(std::unique(next.begin(), next.end()), next.end());
        const auto found = regions_.find(region);
        if (found == regions_.end() && next.empty()) return true;
        if (found != regions_.end() && found->second == next) return true;
        if (found == regions_.end() && regions_.size() >= kMaxRegions) {
            error = "surface topology region budget exceeded"; return false;
        }
        const std::vector<Pair> empty;
        const auto& previous = found == regions_.end() ? empty : found->second;
        std::vector<Pair> added, removed;
        std::set_difference(next.begin(), next.end(), previous.begin(), previous.end(), std::back_inserter(added));
        std::set_difference(previous.begin(), previous.end(), next.begin(), next.end(), std::back_inserter(removed));
        size_t count = references_.size();
        for (const auto& pair : removed) if (references_.at(pair) == 1) --count;
        for (const auto& pair : added) if (!references_.count(pair)) ++count;
        if (count > kMaxPairs) { error = "surface topology pair budget exceeded"; return false; }
        bool changed = false;
        for (const auto& pair : removed) {
            const auto ref = references_.find(pair);
            if (--ref->second == 0) { references_.erase(ref); changed = true; }
        }
        for (const auto& pair : added) if (++references_[pair] == 1) changed = true;
        if (next.empty()) regions_.erase(region);
        else regions_[region] = std::move(next);
        if (changed) ++revision_;
        return true;
    }

    void erase(const Region& region) {
        const auto found = regions_.find(region);
        if (found == regions_.end()) return;
        bool changed = false;
        for (const auto& pair : found->second) {
            const auto ref = references_.find(pair);
            if (--ref->second == 0) { references_.erase(ref); changed = true; }
        }
        regions_.erase(found);
        if (changed) ++revision_;
    }

    void clear() {
        if (!references_.empty()) ++revision_;
        references_.clear(); regions_.clear();
    }

    uint64_t revision() const { return revision_; }
    size_t pair_count() const { return references_.size(); }
    size_t region_count() const { return regions_.size(); }

    // resolve(part, rung) answers the current draw selection. A missing or
    // ambiguous draw has no connection. Query each distinct part once, and
    // never choose a still-cached but unselected rung as a destination.
    template<class Resolver>
    std::vector<VtSurfaceConnectionPair> selected_pairs(Resolver&& resolve) const {
        struct Selection { uint32_t rung = 0; bool valid = false; };
        std::map<uint64_t, Selection> selected;
        const auto selection = [&](uint64_t part) -> const Selection& {
            auto [it, added] = selected.try_emplace(part);
            if (added) it->second.valid = resolve(part, it->second.rung);
            return it->second;
        };
        std::vector<VtSurfaceConnectionPair> out;
        out.reserve(references_.size());
        for (const auto& item : references_) {
            const auto& pair = item.first;
            const auto& a = selection(pair[0]);
            const auto& b = selection(pair[1]);
            if (a.valid && b.valid) out.push_back({pair[0], pair[1], pair[2], a.rung, b.rung});
        }
        return out;
    }

private:
    std::map<Region, std::vector<Pair>> regions_;
    std::map<Pair, uint32_t> references_;
    uint64_t revision_ = 1;
};

} // namespace vt
