/* Linux linker interposition proves acquire/release do not call libc allocators. */
#include "mem_bank.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
void* __real_malloc(size_t);
void* __real_calloc(size_t, size_t);
void* __real_realloc(void*, size_t);
void __real_free(void*);
static unsigned calls;
void* __wrap_malloc(size_t n) { ++calls; return __real_malloc(n); }
void* __wrap_calloc(size_t n, size_t s) { ++calls; return __real_calloc(n,s); }
void* __wrap_realloc(void* p, size_t n) { ++calls; return __real_realloc(p,n); }
void __wrap_free(void* p) { ++calls; __real_free(p); }
int main(void) {
    MemBankConfig config = {1024*1024, 256, 128, 0, NULL};
    MemBank* bank = mem_bank_create(&config);
    MemBankLease leases[128] = {{0}};
    unsigned baseline = calls, random = 42;
    assert(bank);
    for (int i=0; i<100000; ++i) {
        random = random*1664525u+1013904223u;
        unsigned slot = (random>>16)%128;
        if (leases[slot].owner) {
            assert(mem_bank_release(bank, leases[slot]));
            leases[slot].owner = NULL;
        } else {
            mem_bank_acquire(bank, 1+random%32768, &leases[slot]);
            if (leases[slot].owner) {
                for (unsigned j=0; j<128; ++j) {
                    if (slot == j || !leases[j].owner) continue;
                    assert(leases[slot].offset+leases[slot].capacity <= leases[j].offset ||
                           leases[j].offset+leases[j].capacity <= leases[slot].offset);
                }
            }
        }
    }
    for (unsigned i=0; i<128; ++i)
        if (leases[i].owner) assert(mem_bank_release(bank, leases[i]));
    assert(mem_bank_stats(bank).largest_free == config.capacity);
    assert(calls == baseline);
    assert(mem_bank_destroy(bank));
    puts("100,000 randomized bank operations: zero malloc/calloc/realloc/free calls, no overlaps.");
    return 0;
}
