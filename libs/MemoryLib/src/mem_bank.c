#include "mem_bank.h"
#include <stdlib.h>
#include <string.h>

typedef struct Range { size_t offset, capacity, requested; uint64_t generation; } Range;
struct MemBank {
    MemBankConfig config;
    MemBankStats stats;
    Range* ranges;
    void* owned;
    unsigned char* storage;
    uint64_t generation;
};
MemBank* mem_bank_create(const MemBankConfig* c) {
    MemBank* b;
    if (!c || !c->quantum || (c->quantum & (c->quantum-1)) ||
        !c->capacity || c->capacity % c->quantum || !c->max_leases ||
        c->max_leases > SIZE_MAX / sizeof(Range) ||
        c->capacity > SIZE_MAX - (c->quantum-1) ||
        (c->external && c->storage && (uintptr_t)c->storage % c->quantum)) return NULL;
    b = (MemBank*)calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->config = *c;
    b->ranges = (Range*)calloc(c->max_leases, sizeof(Range));
    if (!b->ranges) { free(b); return NULL; }
    b->storage = (unsigned char*)c->storage;
    if (!c->external) {
        b->owned = malloc(c->capacity + c->quantum-1);
        if (!b->owned) { free(b->ranges); free(b); return NULL; }
        b->storage = (unsigned char*)(((uintptr_t)b->owned + c->quantum-1) & ~(uintptr_t)(c->quantum-1));
        memset(b->storage, 0, c->capacity);
        b->stats.backing_allocations = 1;
    }
    b->stats.capacity = c->capacity;
    return b;
}
int mem_bank_destroy(MemBank* b) {
    if (!b) return 1;
    if (b->stats.active) return 0;
    free(b->owned); free(b->ranges); free(b); return 1;
}
int mem_bank_acquire(MemBank* b, size_t bytes, MemBankLease* out) {
    size_t rounded, offset = 0, i;
    if (!b || !out) return 0;
    memset(out, 0, sizeof(*out));
    if (!bytes || bytes > SIZE_MAX - (b->config.quantum-1) ||
        b->stats.active == b->config.max_leases || b->generation == UINT64_MAX) goto fail;
    rounded = (bytes + b->config.quantum-1) & ~(b->config.quantum-1);
    for (i = 0; i < b->stats.active; ++i) {
        if (rounded <= b->ranges[i].offset - offset) break;
        offset = b->ranges[i].offset + b->ranges[i].capacity;
    }
    if (rounded > b->config.capacity - offset) goto fail;
    memmove(b->ranges+i+1, b->ranges+i, (b->stats.active-i)*sizeof(Range));
    b->ranges[i].offset = offset; b->ranges[i].capacity = rounded;
    b->ranges[i].requested = bytes; b->ranges[i].generation = ++b->generation;
    ++b->stats.active; ++b->stats.acquisitions;
    if (b->stats.active > b->stats.peak_active) b->stats.peak_active = b->stats.active;
    b->stats.occupied += rounded; b->stats.requested += bytes;
    out->owner = b; out->generation = b->generation; out->offset = offset;
    out->capacity = rounded; out->data = b->storage ? b->storage+offset : NULL;
    return 1;
fail:
    ++b->stats.failures; return 0;
}
int mem_bank_release(MemBank* b, MemBankLease lease) {
    size_t i;
    if (!b || lease.owner != b || !lease.generation) return 0;
    for (i=0; i<b->stats.active; ++i) {
        Range* r = b->ranges+i;
        if (r->generation != lease.generation) continue;
        if (r->offset != lease.offset || r->capacity != lease.capacity) return 0;
        b->stats.occupied -= r->capacity; b->stats.requested -= r->requested;
        --b->stats.active; ++b->stats.releases;
        memmove(r, r+1, (b->stats.active-i)*sizeof(Range));
        return 1;
    }
    return 0;
}
MemBankStats mem_bank_stats(const MemBank* b) {
    MemBankStats s = {0}; size_t i, end = 0;
    if (!b) return s;
    s = b->stats;
    for (i=0; i<s.active; ++i) {
        size_t gap = b->ranges[i].offset-end;
        if (gap > s.largest_free) s.largest_free = gap;
        end = b->ranges[i].offset+b->ranges[i].capacity;
    }
    if (s.capacity-end > s.largest_free) s.largest_free = s.capacity-end;
    return s;
}
