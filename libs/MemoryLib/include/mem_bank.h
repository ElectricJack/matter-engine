#ifndef MEM_BANK_H
#define MEM_BANK_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Thread-confined fixed capacity range allocator. Creation reserves and touches
 * owned storage and metadata; acquire/release never allocate backing storage.
 * External storage (including NULL for offset-only GPU banks) is never freed.
 * Capacity and allocations are multiples of quantum, a power of two. */
typedef struct MemBank MemBank;
typedef struct MemBankConfig {
    size_t capacity, quantum, max_leases;
    int external;
    void* storage;
} MemBankConfig;
typedef struct MemBankLease {
    MemBank* owner;
    uint64_t generation;
    size_t offset, capacity;
    void* data;
} MemBankLease;
typedef struct MemBankStats {
    size_t capacity, occupied, requested, largest_free, active, peak_active;
    uint64_t acquisitions, releases, failures, backing_allocations;
} MemBankStats;
MemBank* mem_bank_create(const MemBankConfig* config);
/* Returns zero while leases remain live; leaves bank intact. NULL is valid. */
int mem_bank_destroy(MemBank* bank);
int mem_bank_acquire(MemBank* bank, size_t bytes, MemBankLease* lease);
/* Stale/foreign/double releases fail without modifying the bank. */
int mem_bank_release(MemBank* bank, MemBankLease lease);
MemBankStats mem_bank_stats(const MemBank* bank);
#ifdef __cplusplus
}
#endif
#endif
