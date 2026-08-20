#ifndef MEMORY_HPP
#define MEMORY_HPP

// libs/MemoryLib/include/memory.hpp
//
// Header-only C++ RAII wrappers over MemoryLib's C handles: `mem::Arena`
// around MemArena and `mem::Pool` around MemPool. They add ownership and
// compile-time safety checks; they add no allocation policy of their own —
// every call forwards straight to the C function, so mem_arena.h and
// mem_pool.h remain the authority on behavior.
//
// Where it fits
// -------------
// MemoryLib is the bottom of the dependency chain and depends on nothing but
// libc and <type_traits>. Consumers add -I to this directory and compile
// libs/MemoryLib/src/mem_arena.c and/or mem_pool.c from where they live; this
// header alone is not enough to link. Today the only consumer is MemoryLib's
// own tests/memory_hpp_tests.cpp — engine code uses the C API directly.
//
// Considerations
// -------------
// - Both types are move-only: copy construction and copy assignment are
//   deleted, and a moved-from object holds a null handle.
// - Construction cannot throw and cannot report failure through the
//   constructor. If the underlying create() fails (out of memory, or a zero
//   argument), the object is constructed with a null handle. Call `valid()`
//   before using it.
// - A moved-from or invalid object stays safe to touch: the destructor,
//   alloc() and Pool::free() all forward a null handle to C functions that
//   tolerate NULL. `stats()` is the exception — see its comment.
// - Thread-confined, exactly like the C allocators underneath: one instance
//   belongs to one thread, and nothing here takes a lock.
// - Arena memory is never destructed. `Arena::allocArray<T>` enforces that
//   at compile time, along with the arena's 8-byte alignment ceiling.

#include <cstddef>
#include <type_traits>
#include "mem_arena.h"
#include "mem_pool.h"

namespace mem {

// Owns exactly one MemArena for its lifetime and destroys it in ~Arena.
// Constructed with the first block's capacity in bytes; a failed create
// leaves the handle null, which only valid() reports.
//
// Lifetime rule: pointers obtained from alloc()/allocArray() stay valid until
// reset(), until the Arena is destroyed, or until it is moved FROM (the
// moved-from object's handle goes null and the new owner may reset it). They
// survive further allocations, since arena blocks are never relocated.
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

    // Returns space for `count` T's, uninitialized — no constructors run, and
    // no destructors will ever run either (hence the trivially-destructible
    // requirement). Returns nullptr on out-of-memory, on a null handle, on
    // count == 0, and when count * sizeof(T) would overflow size_t.
    template <typename T>
    T* allocArray(size_t count) {
        static_assert(std::is_trivially_destructible<T>::value,
                      "arena memory is never destructed");
        static_assert(alignof(T) <= 8, "arena guarantees 8-byte alignment only");
        if (count > static_cast<size_t>(-1) / sizeof(T)) {
            return nullptr;
        }
        return static_cast<T*>(mem_arena_alloc(a_, count * sizeof(T)));
    }

    // reset() invalidates every pointer this Arena has handed out; it keeps
    // the largest block for reuse and does not zero it.
    //
    // stats() is only meaningful when valid() is true: mem_arena_get_stats()
    // returns without writing anything for a null handle, so calling stats()
    // on a default-failed or moved-from Arena returns an uninitialized
    // MemStats. Guard with valid().
    void reset() { mem_arena_reset(a_); }
    MemStats stats() const { MemStats s; mem_arena_get_stats(a_, &s); return s; }
    bool valid() const { return a_ != nullptr; }

private:
    MemArena* a_;
};

// Owns exactly one MemPool for its lifetime and destroys it in ~Pool.
// Constructed with the object size in bytes and the objects-per-page
// granularity; a failed create (either argument 0, or out of memory) leaves
// the handle null, which only valid() reports.
//
// ~Pool frees every page whether or not objects in them are still live, and
// runs no destructors — this wrapper manages the pool's lifetime, not the
// lifetime of the objects it hands out.
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

    // alloc() returns uninitialized storage, or nullptr on out-of-memory / a
    // null handle. free() takes a pointer this same Pool returned; ownership
    // is not tracked, so a foreign or double free silently corrupts the free
    // list. Neither runs a constructor or destructor.
    //
    // stats(), like Arena::stats(), returns an uninitialized MemStats when
    // valid() is false, because the C getter skips a null handle.
    void* alloc() { return mem_pool_alloc(p_); }
    void free(void* obj) { mem_pool_free(p_, obj); }
    MemStats stats() const { MemStats s; mem_pool_get_stats(p_, &s); return s; }
    bool valid() const { return p_ != nullptr; }

private:
    MemPool* p_;
};

} // namespace mem

#endif // MEMORY_HPP
