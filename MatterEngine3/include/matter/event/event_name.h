// matter/event/event_name.h — MT_EVENT_NAME, the compile-time event
// descriptor macro.
//
// Design authority: MatterEngine3/docs/event-system.md S I.5 "Declaring an
// event type": "MT_EVENT_NAME expands to a static constexpr name + a
// stable type id."
//
// Usage (inside a plain-struct event payload, per S I.5 rule 1):
//
//   struct PartBaked {
//       MT_EVENT_NAME("bake.part_done");
//       std::string module;
//       int done = 0, total = 0;
//   };
//
// Type-id mechanism (documented per the milestone brief's requirement to
// pick one and explain it): the address of a function-local static byte
// inside a member function (mt_event_type_id()) that the macro writes
// directly into the enclosing struct's body. Because that function body
// is emitted once per struct that invokes the macro — never through a
// class template — each invocation gets its own distinct function and
// therefore its own distinct static-storage-duration object. The
// function's address-of-local-static is:
//   - unique per event type (two different structs never share the byte),
//   - stable for the life of the process (static storage duration),
//   - ODR-safe across translation units (the function is an ordinary
//     inline member function; every TU that sees the struct definition
//     gets the same function and therefore the same static object once
//     linked),
//   - free of RTTI/typeid() (no -frtti dependency, no typeid comparison
//     subtleties across shared-library boundaries).
// This gives the Hub's per-event-type registry a stable `const void*` key
// without relying on std::type_index/typeid, while `mt_event_name` (the
// dotted string) remains the human-facing registry/trace/ordering key
// that events.md promises.
#pragma once

#define MT_EVENT_NAME(name_literal)                                 \
    static constexpr const char* mt_event_name = (name_literal);    \
    static const void* mt_event_type_id() {                         \
        static const char marker = 0;                               \
        return static_cast<const void*>(&marker);                   \
    }                                                                \
    static_assert(true, "MT_EVENT_NAME requires a trailing semicolon")
