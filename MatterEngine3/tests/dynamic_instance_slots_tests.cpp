// dynamic_instance_slots_tests.cpp — Phase 4 Task 6: CPU stable dynamic instance
// slots. Pure CPU test: no Flecs, no physics, no GL.
#include "check.h"
#include "render/dynamic_instance_slots.h"

#include <cstdio>

using matter::render::DynamicInstanceInput;
using matter::render::DynamicInstanceSlots;
using matter::render::DynamicSlotChangeKind;
using matter::render::DynamicSlotHandle;
using matter::render::SlotResult;

namespace {

matter::Mat4f identity() {
    matter::Mat4f m{};
    m.m[0] = m.m[5] = m.m[10] = m.m[15] = 1.0f;
    return m;
}

matter::Mat4f translated(float x) {
    matter::Mat4f m = identity();
    m.m[12] = x;
    return m;
}

matter::scene::SceneEntityId entity(uint64_t v) {
    matter::scene::SceneEntityId id;
    id.value = v;
    return id;
}

void test_bind_on_insert() {
    DynamicInstanceSlots slots(4);
    DynamicInstanceInput in;
    in.id = entity(1);
    in.part_hash = 100;
    in.object_to_world = identity();
    in.casts_shadow = true;

    auto result = slots.upsert(in);
    CHECK(result.result == SlotResult::Ok, "bind_on_insert: upsert ok");
    CHECK(result.handle.valid(), "bind_on_insert: handle valid");
    CHECK(slots.active_count() == 1, "bind_on_insert: active count 1");

    auto changes = slots.drain();
    CHECK(changes.size() == 1, "bind_on_insert: one change emitted");
    if (changes.size() == 1) {
        CHECK(changes[0].kind == DynamicSlotChangeKind::Bind, "bind_on_insert: kind is Bind");
        CHECK(changes[0].part_hash == 100, "bind_on_insert: part_hash matches");
        CHECK(changes[0].entity_id.value == 1, "bind_on_insert: entity id matches");
    }
}

void test_noop_upsert() {
    DynamicInstanceSlots slots(4);
    DynamicInstanceInput in;
    in.id = entity(1);
    in.part_hash = 100;
    in.object_to_world = identity();
    in.casts_shadow = true;

    slots.upsert(in);
    slots.drain(); // discard bind change

    auto result = slots.upsert(in); // identical values
    CHECK(result.result == SlotResult::Ok, "noop_upsert: ok");
    auto changes = slots.drain();
    CHECK(changes.empty(), "noop_upsert: drain empty on identical upsert");
}

void test_transform_change() {
    DynamicInstanceSlots slots(4);
    DynamicInstanceInput in;
    in.id = entity(1);
    in.part_hash = 100;
    in.object_to_world = identity();
    in.casts_shadow = true;

    slots.upsert(in);
    slots.drain();

    in.object_to_world = translated(5.0f);
    auto result = slots.upsert(in);
    CHECK(result.result == SlotResult::Ok, "transform_change: ok");

    auto changes = slots.drain();
    CHECK(changes.size() == 1, "transform_change: one change emitted");
    if (changes.size() == 1) {
        CHECK(changes[0].kind == DynamicSlotChangeKind::Transform,
              "transform_change: kind is Transform");
        CHECK(changes[0].object_to_world.m[12] == 5.0f, "transform_change: transform updated");
    }
}

void test_part_change_emits_bind() {
    DynamicInstanceSlots slots(4);
    DynamicInstanceInput in;
    in.id = entity(1);
    in.part_hash = 100;
    in.object_to_world = identity();
    in.casts_shadow = true;

    slots.upsert(in);
    slots.drain();

    in.part_hash = 200;
    auto result = slots.upsert(in);
    CHECK(result.result == SlotResult::Ok, "part_change: ok");

    auto changes = slots.drain();
    CHECK(changes.size() == 1, "part_change: one change emitted");
    if (changes.size() == 1) {
        CHECK(changes[0].kind == DynamicSlotChangeKind::Bind, "part_change: kind is Bind");
        CHECK(changes[0].part_hash == 200, "part_change: part_hash updated");
    }
}

void test_remove_emits_change_and_stale_handle() {
    DynamicInstanceSlots slots(4);
    DynamicInstanceInput in;
    in.id = entity(1);
    in.part_hash = 100;
    in.object_to_world = identity();
    in.casts_shadow = true;

    auto up = slots.upsert(in);
    slots.drain();

    DynamicSlotHandle handle = up.handle;
    SlotResult rm = slots.remove(handle);
    CHECK(rm == SlotResult::Ok, "remove: ok");
    CHECK(slots.active_count() == 0, "remove: active count back to 0");

    auto changes = slots.drain();
    CHECK(changes.size() == 1, "remove: one change emitted");
    if (changes.size() == 1) {
        CHECK(changes[0].kind == DynamicSlotChangeKind::Remove, "remove: kind is Remove");
        CHECK(changes[0].entity_id.value == 1, "remove: entity id matches");
    }

    // Using the same (now stale) handle again must fail.
    SlotResult rm_again = slots.remove(handle);
    CHECK(rm_again == SlotResult::StaleGeneration, "remove: stale handle rejected");
}

void test_capacity_exhausted() {
    DynamicInstanceSlots slots(2);
    DynamicInstanceInput a;
    a.id = entity(1);
    a.part_hash = 1;
    a.object_to_world = identity();

    DynamicInstanceInput b;
    b.id = entity(2);
    b.part_hash = 2;
    b.object_to_world = identity();

    DynamicInstanceInput c;
    c.id = entity(3);
    c.part_hash = 3;
    c.object_to_world = identity();

    CHECK(slots.upsert(a).result == SlotResult::Ok, "capacity: first insert ok");
    CHECK(slots.upsert(b).result == SlotResult::Ok, "capacity: second insert ok");

    auto result = slots.upsert(c);
    CHECK(result.result == SlotResult::CapacityExhausted, "capacity: third insert exhausted");
    CHECK(!result.handle.valid(), "capacity: handle invalid on exhaustion");
    CHECK(slots.active_count() == 2, "capacity: active count unaffected by failed insert");
}

void test_deferred_reuse() {
    DynamicInstanceSlots slots(1);
    DynamicInstanceInput a;
    a.id = entity(1);
    a.part_hash = 1;
    a.object_to_world = identity();

    auto up = slots.upsert(a);
    slots.drain();

    // Advance current_serial_ past 0 so the retire tag is distinguishable
    // from the initial "no work done yet" completed_serial of 0.
    slots.finish_frame(0); // no pending work yet; advances internal serial to 1

    SlotResult rm = slots.remove(up.handle);
    CHECK(rm == SlotResult::Ok, "deferred_reuse: remove ok");
    slots.drain();

    DynamicInstanceInput b;
    b.id = entity(2);
    b.part_hash = 2;
    b.object_to_world = identity();

    // Slot not yet reusable: completed_serial 0 is behind the retire serial (1).
    auto blocked = slots.upsert(b);
    CHECK(blocked.result == SlotResult::CapacityExhausted,
          "deferred_reuse: not reusable before finish_frame advances");

    // finish_frame with a completed_serial behind the retire serial keeps it blocked.
    slots.finish_frame(0);
    auto still_blocked = slots.upsert(b);
    CHECK(still_blocked.result == SlotResult::CapacityExhausted,
          "deferred_reuse: still blocked after finish_frame(0)");

    // finish_frame reaching the retire serial frees the slot for reuse.
    slots.finish_frame(1);
    auto reused = slots.upsert(b);
    CHECK(reused.result == SlotResult::Ok, "deferred_reuse: reusable after finish_frame advances");
    CHECK(reused.handle.valid(), "deferred_reuse: reused handle valid");
    CHECK(slots.active_count() == 1, "deferred_reuse: active count reflects reused slot");

    auto changes = slots.drain();
    CHECK(changes.size() == 1, "deferred_reuse: reuse emits a Bind change");
    if (changes.size() == 1) {
        CHECK(changes[0].kind == DynamicSlotChangeKind::Bind, "deferred_reuse: reuse kind is Bind");
        CHECK(changes[0].entity_id.value == 2, "deferred_reuse: reuse entity id matches new entity");
    }
}

void test_drain_clears_buffer() {
    DynamicInstanceSlots slots(4);
    DynamicInstanceInput in;
    in.id = entity(1);
    in.part_hash = 1;
    in.object_to_world = identity();

    slots.upsert(in);
    auto first = slots.drain();
    CHECK(first.size() == 1, "drain_clears: first drain has the bind change");

    auto second = slots.drain();
    CHECK(second.empty(), "drain_clears: second drain is empty");
}

} // namespace

int main() {
    test_bind_on_insert();
    test_noop_upsert();
    test_transform_change();
    test_part_change_emits_bind();
    test_remove_emits_change_and_stale_handle();
    test_capacity_exhausted();
    test_deferred_reuse();
    test_drain_clears_buffer();
    return check_summary();
}
