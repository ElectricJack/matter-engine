#ifndef MEMORY_HPP
#define MEMORY_HPP

#include <cstddef>
#include <type_traits>
#include "mem_arena.h"
#include "mem_pool.h"

namespace mem {

// RAII over MemArena. Move-only. Arena never runs destructors: POD-ish types only.
class Arena {
public:
    explicit Arena(size_t initialCap) : a_(mem_arena_create(initialCap)) {}
    ~Arena() { mem_arena_destroy(a_); }
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    Arena(Arena&& o) noexcept : a_(o.a_) { o.a_ = nullptr; }
    Arena& operator=(Arena&& o) noexcept {
        if (this != &o) {
            mem_arena_destroy(a_);
            a_ = o.a_;
            o.a_ = nullptr;
        }
        return *this;
    }

    void* alloc(size_t n) { return mem_arena_alloc(a_, n); }

    template <typename T>
    T* allocArray(size_t count) {
        static_assert(std::is_trivially_destructible<T>::value,
                      "arena memory is never destructed");
        static_assert(alignof(T) <= 8, "arena guarantees 8-byte alignment only");
        return static_cast<T*>(mem_arena_alloc(a_, count * sizeof(T)));
    }

    void reset() { mem_arena_reset(a_); }
    MemStats stats() const { MemStats s; mem_arena_get_stats(a_, &s); return s; }
    bool valid() const { return a_ != nullptr; }

private:
    MemArena* a_;
};

// RAII over MemPool. Move-only.
class Pool {
public:
    Pool(size_t objectSize, size_t objectsPerPage)
        : p_(mem_pool_create(objectSize, objectsPerPage)) {}
    ~Pool() { mem_pool_destroy(p_); }
    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;
    Pool(Pool&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    Pool& operator=(Pool&& o) noexcept {
        if (this != &o) {
            mem_pool_destroy(p_);
            p_ = o.p_;
            o.p_ = nullptr;
        }
        return *this;
    }

    void* alloc() { return mem_pool_alloc(p_); }
    void free(void* obj) { mem_pool_free(p_, obj); }
    MemStats stats() const { MemStats s; mem_pool_get_stats(p_, &s); return s; }
    bool valid() const { return p_ != nullptr; }

private:
    MemPool* p_;
};

} // namespace mem

#endif // MEMORY_HPP
