/* Fault injection shared by parser and public compiler regressions. */
#ifndef MATTER_TEST_OOM_ALLOCATOR_H
#define MATTER_TEST_OOM_ALLOCATOR_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef union Block {
#ifdef _MSC_VER
    /* MSVC's C headers omit max_align_t; x64 malloc is 16-byte aligned. */
    __declspec(align(16)) unsigned char alignment[16];
#else
    max_align_t alignment;
#endif
    size_t size;
} Block;

typedef struct Allocator {
    size_t calls, fail_at, live;
    int armed, failed, all_allocations;
} Allocator;

static int reject(Allocator *a)
{
    if (!a->armed)
        return 0;
    if (++a->calls != a->fail_at)
        return 0;
    a->failed = 1;
    return 1;
}

static void *allocate_raw(void *opaque, size_t size)
{
    Allocator *a = opaque;
    Block *b;
    if (size > SIZE_MAX - sizeof(*b))
        return NULL;
    b = malloc(sizeof(*b) + size);
    if (!b)
        return NULL;
    b->size = size;
    ++a->live;
    return b + 1;
}

static void *allocate(void *opaque, size_t size)
{
    Allocator *a = opaque;
    if (a->all_allocations && reject(a))
        return NULL;
    return allocate_raw(opaque, size);
}

static void release(void *opaque, void *ptr)
{
    Allocator *a = opaque;
    if (ptr) {
        --a->live;
        free((Block *)ptr - 1);
    }
}

static void *resize(void *opaque, void *ptr, size_t size)
{
    Allocator *a = opaque;
    Block *b;
    if (!ptr) {
        if (reject(a))
            return NULL;
        return allocate_raw(opaque, size);
    }
    if (!size) {
        release(opaque, ptr);
        return NULL;
    }
    b = (Block *)ptr - 1;
    /* Failed optional shrinking is allowed by QuickJS; target growth only. */
    if ((size > b->size && reject(a)) || size > SIZE_MAX - sizeof(*b))
        return NULL;
    b = realloc(b, sizeof(*b) + size);
    if (!b)
        return NULL;
    b->size = size;
    return b + 1;
}

static void *zero_allocate(void *opaque, size_t count, size_t size)
{
    void *ptr;
    if (size && count > SIZE_MAX / size)
        return NULL;
    ptr = allocate(opaque, count * size);
    if (ptr)
        memset(ptr, 0, count * size);
    return ptr;
}

static size_t allocation_size(const void *ptr)
{
    return ptr ? ((const Block *)ptr - 1)->size : 0;
}

#endif
