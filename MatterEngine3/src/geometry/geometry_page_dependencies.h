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
}
