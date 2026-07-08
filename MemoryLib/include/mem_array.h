#ifndef MEM_ARRAY_H
#define MEM_ARRAY_H

#include <stddef.h>
#include "mem_stats.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Growable array (heap-backed). Thread-confined. */
typedef struct MemArray {
    void*  data;
    size_t count;
    size_t capacity;    /* in elements */
    size_t elemSize;
    size_t growCount;   /* number of reallocs; reported as stats totalAllocs */
} MemArray;

void  mem_array_init(MemArray* arr, size_t elemSize);
int   mem_array_ensure(MemArray* arr, size_t minCapacity);  /* 1 ok, 0 OOM (data intact) */
void* mem_array_push(MemArray* arr);                        /* new slot, NULL on OOM */
void  mem_array_clear(MemArray* arr);                       /* count=0, keeps capacity */
void  mem_array_free(MemArray* arr);
void  mem_array_get_stats(const MemArray* arr, MemStats* out);

#ifdef __cplusplus
}
#endif

#endif /* MEM_ARRAY_H */
