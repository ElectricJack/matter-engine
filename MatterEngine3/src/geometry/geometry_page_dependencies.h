#pragma once
#include "asset_store.h"
#include <vector>

namespace geometry {
// Both lists must be sorted by BlobHash::operator<. Duplicates are allowed.
// Contiguous merge avoids tree probes for every newly published page.
inline bool page_sets_intersect(const std::vector<asset_store::BlobHash>& a,
                                const std::vector<asset_store::BlobHash>& b) {
    size_t i=0,j=0;
    while(i<a.size() && j<b.size()) {
        if(a[i]<b[j]) ++i;
        else if(b[j]<a[i]) ++j;
        else return true;
    }
    return false;
}
// Publication changes a displayed cut only when one of its dependencies
// changes (including an async replacement's frontier), or an ordinary source
// fallback gains its first complete root coverage. Partial roots of another
// asset must not force a repack of every already-displayed instance.
inline bool cut_publication_changed(const std::vector<asset_store::BlobHash>& published,
                                    const std::vector<asset_store::BlobHash>& displayed,
                                    const std::vector<asset_store::BlobHash>& pending,
                                    bool first_coverage_ready) {
    return !published.empty() && (first_coverage_ready ||
        page_sets_intersect(published, displayed) || page_sets_intersect(published, pending));
}
}
