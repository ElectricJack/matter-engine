#include "../include/mem_array.h"
#include <stdlib.h>
#include <string.h>

void mem_array_init(MemArray* arr, size_t elemSize) {
    arr->data = NULL;
    arr->count = 0;
    arr->capacity = 0;
    arr->elemSize = elemSize;
    arr->growCount = 0;
}

int mem_array_ensure(MemArray* arr, size_t minCapacity) {
    if (minCapacity <= arr->capacity) {
        return 1;
    }
    size_t newCap = arr->capacity + arr->capacity / 2;   /* 1.5x */
    if (newCap < minCapacity) {
        newCap = minCapacity;
    }
    if (newCap < 16) {
        newCap = 16;
    }
    void* tmp = realloc(arr->data, newCap * arr->elemSize);
    if (!tmp) {
        return 0;   /* old data intact */
    }
    arr->data = tmp;
    arr->capacity = newCap;
    arr->growCount++;
    return 1;
}

void* mem_array_push(MemArray* arr) {
    if (arr->count == arr->capacity && !mem_array_ensure(arr, arr->count + 1)) {
        return NULL;
    }
    return (char*)arr->data + arr->count++ * arr->elemSize;
}

void mem_array_clear(MemArray* arr) {
    arr->count = 0;
}

void mem_array_free(MemArray* arr) {
    free(arr->data);
    arr->data = NULL;
    arr->count = 0;
    arr->capacity = 0;
    arr->growCount = 0;
}

void mem_array_get_stats(const MemArray* arr, MemStats* out) {
    if (!arr || !out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->liveBytes = arr->count * arr->elemSize;
    out->peakBytes = arr->capacity * arr->elemSize;   /* capacity never shrinks */
    out->totalAllocs = arr->growCount;
}
