// scene_registry_tests.cpp — Phase 4 Task 4: reflected ECS scene registry.

#include "check.h"
#include "matter/ecs.h"
#include "matter/physics.h"
#include "matter/scene.h"
#include "matter/streaming.h"
#include "matter/world_definition.h"
#include "ecs/scene_registry.h"

#include "flecs.h"

#include <string>
#include <vector>

using namespace matter;
using namespace matter::scene;

// ---------------------------------------------------------------------------
// Reflection registration tests.
// ---------------------------------------------------------------------------

static void test_scene_module_registers_scene_entity_id() {
    flecs::world world;
    world.import<ecs::CoreModule>();
    world.import<physics::PhysicsModule>();
    world.import<SceneModule>();

    auto comp = world.component<SceneEntityId>();
    CHECK(comp.id() != 0, "SceneEntityId not registered");
}

static void test_scene_module_registers_part_instance() {
    flecs::world world;
    world.import<ecs::CoreModule>();
    world.import<physics::PhysicsModule>();
    world.import<SceneModule>();

    auto comp = world.component<PartInstance>();
    CHECK(comp.id() != 0, "PartInstance not registered");
}

static void test_scene_module_registers_part_instance_error() {
    flecs::world world;
    world.import<ecs::CoreModule>();
    world.import<physics::PhysicsModule>();
    world.import<SceneModule>();

    auto comp = world.component<PartInstanceError>();
    CHECK(comp.id() != 0, "PartInstanceError not registered");
}

static void test_core_module_reflects_transform() {
    flecs::world world;
    world.import<ecs::CoreModule>();

    auto comp = world.component<ecs::LocalTransform>();
    CHECK(comp.id() != 0, "LocalTransform not registered");
}

static void test_physics_module_reflects_rigid_body() {
    flecs::world world;
    world.import<ecs::CoreModule>();
    world.import<physics::PhysicsModule>();

    auto comp = world.component<physics::RigidBody>();
    CHECK(comp.id() != 0, "RigidBody not registered");
}

static void test_physics_module_reflects_velocity() {
    flecs::world world;
    world.import<ecs::CoreModule>();
    world.import<physics::PhysicsModule>();

    auto comp = world.component<physics::PhysicsVelocity>();
    CHECK(comp.id() != 0, "PhysicsVelocity not registered");
}

static void test_physics_module_reflects_sphere_collider() {
    flecs::world world;
    world.import<ecs::CoreModule>();
    world.import<physics::PhysicsModule>();

    auto comp = world.component<physics::SphereCollider>();
    CHECK(comp.id() != 0, "SphereCollider not registered");
}

static void test_physics_module_reflects_capsule_collider() {
    flecs::world world;
    world.import<ecs::CoreModule>();
    world.import<physics::PhysicsModule>();

    auto comp = world.component<physics::CapsuleCollider>();
    CHECK(comp.id() != 0, "CapsuleCollider not registered");
}

static void test_physics_module_reflects_box_collider() {
    flecs::world world;
    world.import<ecs::CoreModule>();
    world.import<physics::PhysicsModule>();

    auto comp = world.component<physics::BoxCollider>();
    CHECK(comp.id() != 0, "BoxCollider not registered");
}

static void test_physics_module_reflects_convex_hull_collider() {
    flecs::world world;
    world.import<ecs::CoreModule>();
    world.import<physics::PhysicsModule>();

    auto comp = world.component<physics::ConvexHullCollider>();
    CHECK(comp.id() != 0, "ConvexHullCollider not registered");
}

static void test_streaming_module_reflects_sector_streaming() {
    flecs::world world;
    world.import<ecs::CoreModule>();
    world.import<streaming::StreamingModule>();

    auto comp = world.component<streaming::SectorStreaming>();
    CHECK(comp.id() != 0, "SectorStreaming not registered");
}

// ---------------------------------------------------------------------------
// Component descriptor lookup tests.
// ---------------------------------------------------------------------------

static void test_find_component_known() {
    CHECK(find_component("LocalTransform") != nullptr, "LocalTransform not found");
    CHECK(find_component("RigidBody") != nullptr, "RigidBody not found");
    CHECK(find_component("PhysicsVelocity") != nullptr, "PhysicsVelocity not found");
    CHECK(find_component("SphereCollider") != nullptr, "SphereCollider not found");
    CHECK(find_component("CapsuleCollider") != nullptr, "CapsuleCollider not found");
    CHECK(find_component("BoxCollider") != nullptr, "BoxCollider not found");
    CHECK(find_component("ConvexHullCollider") != nullptr, "ConvexHullCollider not found");
    CHECK(find_component("PartInstance") != nullptr, "PartInstance not found");
    CHECK(find_component("SectorStreaming") != nullptr, "SectorStreaming not found");
}

static void test_find_component_unknown() {
    CHECK(find_component("Nonexistent") == nullptr, "Nonexistent should be null");
    CHECK(find_component("MeshRenderer") == nullptr, "MeshRenderer should be null");
}

static void test_component_count() {
    CHECK(component_count() == 9, "expected 9 registered components");
}

// ---------------------------------------------------------------------------
// Descriptor offset/metadata tests (property-system spec S7).
//
// The tables build their offsets with offsetof, so a renamed or reordered
// member is a compile error. What a table can still get wrong is the field
// TYPE (calling a uint32_t member a Float, say) or the storage width of an
// integral member. These checks write through the descriptor's offset and
// read back through the real typed member, one field per component plus every
// field whose storage is unusual — which is exactly what catches that drift.
// ---------------------------------------------------------------------------

static const FieldDescriptor* field_of(const char* component, const char* field) {
    const ComponentDescriptor* cd = find_component(component);
    if (!cd) return nullptr;
    return find_field(*cd, field);
}

static void test_find_field() {
    CHECK(field_of("RigidBody", "gravity_scale") != nullptr, "gravity_scale not found");
    CHECK(field_of("RigidBody", "nonexistent") == nullptr, "unknown field should be null");
    CHECK(field_of("SectorStreaming", "anything") == nullptr, "tag component has no fields");
    const ComponentDescriptor* cd = find_component("RigidBody");
    CHECK(find_field(*cd, nullptr) == nullptr, "null field name should be null");
}

static void test_component_struct_sizes_match() {
    struct Expect { const char* name; uint32_t size; uint32_t align; };
    const Expect expected[] = {
        {"LocalTransform", sizeof(ecs::LocalTransform), alignof(ecs::LocalTransform)},
        {"RigidBody", sizeof(physics::RigidBody), alignof(physics::RigidBody)},
        {"PhysicsVelocity", sizeof(physics::PhysicsVelocity), alignof(physics::PhysicsVelocity)},
        {"SphereCollider", sizeof(physics::SphereCollider), alignof(physics::SphereCollider)},
        {"CapsuleCollider", sizeof(physics::CapsuleCollider), alignof(physics::CapsuleCollider)},
        {"BoxCollider", sizeof(physics::BoxCollider), alignof(physics::BoxCollider)},
        {"ConvexHullCollider", sizeof(physics::ConvexHullCollider), alignof(physics::ConvexHullCollider)},
        {"PartInstance", sizeof(PartInstance), alignof(PartInstance)},
        {"SectorStreaming", sizeof(streaming::SectorStreaming), alignof(streaming::SectorStreaming)},
    };
    for (const auto& e : expected) {
        const ComponentDescriptor* cd = find_component(e.name);
        CHECK(cd != nullptr, "component missing");
        if (!cd) continue;
        CHECK(cd->struct_size == e.size, "struct_size mismatch");
        CHECK(cd->struct_align == e.align, "struct_align mismatch");
        CHECK(cd->struct_size <= kMaxComponentStructSize,
              "component larger than kMaxComponentStructSize");
        CHECK(cd->struct_align <= kMaxComponentStructAlign,
              "component alignment above kMaxComponentStructAlign");
    }
}

static void test_every_field_offset_is_in_bounds() {
    for (uint32_t i = 0; i < component_count(); ++i) {
        const ComponentDescriptor* cd = component_at(i);
        CHECK(cd != nullptr, "component_at returned null");
        if (!cd) continue;
        for (uint32_t f = 0; f < cd->field_count; ++f) {
            const FieldDescriptor& fd = cd->fields[f];
            CHECK(fd.name != nullptr, "field without a name");
            CHECK(fd.offset < cd->struct_size, "field offset past the end of the struct");
            if (fd.type == FieldType::Enum)
                CHECK(fd.enum_labels != nullptr && fd.enum_count > 0,
                      "enum field without labels");
        }
    }
}

static void test_transform_offsets() {
    ecs::LocalTransform t{};
    CHECK(field_set_float3(&t, *field_of("LocalTransform", "translation"), Float3{1, 2, 3}),
          "translation set failed");
    CHECK(t.translation.x == 1.0f && t.translation.y == 2.0f && t.translation.z == 3.0f,
          "translation offset/type drift");

    CHECK(field_set_quat(&t, *field_of("LocalTransform", "rotation"),
                         Quaternion{0.1f, 0.2f, 0.3f, 0.4f}), "rotation set failed");
    CHECK(t.rotation.x == 0.1f && t.rotation.w == 0.4f, "rotation offset/type drift");

    CHECK(field_set_float3(&t, *field_of("LocalTransform", "scale"), Float3{4, 5, 6}),
          "scale set failed");
    CHECK(t.scale.z == 6.0f, "scale offset/type drift");

    Float3 got{};
    CHECK(field_get_float3(&t, *field_of("LocalTransform", "translation"), got),
          "translation get failed");
    CHECK(got.y == 2.0f, "translation read-back drift");

    // A type mismatch must fail rather than reinterpret the bytes.
    float f = 0.0f;
    CHECK(!field_get_float(&t, *field_of("LocalTransform", "translation"), f),
          "float read of a Float3 field should fail");
    Quaternion q{};
    CHECK(!field_get_quat(&t, *field_of("LocalTransform", "scale"), q),
          "quat read of a Float3 field should fail");
}

static void test_rigid_body_offsets() {
    physics::RigidBody rb{};
    // 1-byte enum storage: the width has to come from the descriptor.
    const FieldDescriptor* type_fd = field_of("RigidBody", "type");
    CHECK(type_fd->storage_size == sizeof(physics::RigidBodyType), "enum storage width drift");
    CHECK(field_set_int(&rb, *type_fd, 2), "type set failed");
    CHECK(rb.type == physics::RigidBodyType::Dynamic, "type offset/width drift");
    int32_t iv = 0;
    CHECK(field_get_int(&rb, *type_fd, iv) && iv == 2, "type read-back drift");
    // Clamped to the label index span, as the hand-written setter did.
    CHECK(field_set_int(&rb, *type_fd, 99), "type set (out of range) failed");
    CHECK(rb.type == physics::RigidBodyType::Dynamic, "enum write should clamp to 2");
    CHECK(field_set_int(&rb, *type_fd, -5), "type set (negative) failed");
    CHECK(rb.type == physics::RigidBodyType::Static, "enum write should clamp to 0");

    CHECK(field_set_float(&rb, *field_of("RigidBody", "gravity_scale"), -2.5f),
          "gravity_scale set failed");
    CHECK(rb.gravity_scale == -2.5f, "gravity_scale offset/type drift");
    CHECK(field_set_float(&rb, *field_of("RigidBody", "sleep_threshold"), 0.25f),
          "sleep_threshold set failed");
    CHECK(rb.sleep_threshold == 0.25f, "sleep_threshold offset/type drift");
    CHECK(field_set_bool(&rb, *field_of("RigidBody", "enable_sleep"), false),
          "enable_sleep set failed");
    CHECK(rb.enable_sleep == false, "enable_sleep offset/type drift");
    CHECK(field_set_bool(&rb, *field_of("RigidBody", "continuous"), true),
          "continuous set failed");
    CHECK(rb.continuous == true, "continuous offset/type drift");
}

static void test_rigid_body_type_labels() {
    const FieldDescriptor* fd = field_of("RigidBody", "type");
    CHECK(fd->type == FieldType::Enum, "RigidBody.type should be an enum field");
    CHECK(fd->enum_count == 3, "RigidBody.type should carry 3 labels");
    CHECK(std::string(fd->enum_labels[0]) == "Static", "label 0 should be Static");
    CHECK(std::string(fd->enum_labels[1]) == "Kinematic", "label 1 should be Kinematic");
    CHECK(std::string(fd->enum_labels[2]) == "Dynamic", "label 2 should be Dynamic");
}

static void test_velocity_offsets() {
    physics::PhysicsVelocity v{};
    CHECK(field_set_float3(&v, *field_of("PhysicsVelocity", "linear"), Float3{1, 0, 0}),
          "linear set failed");
    CHECK(v.linear.x == 1.0f, "linear offset/type drift");
    CHECK(field_set_float3(&v, *field_of("PhysicsVelocity", "angular"), Float3{0, 0, 7}),
          "angular set failed");
    CHECK(v.angular.z == 7.0f, "angular offset/type drift");
}

static void test_collider_offsets() {
    // The colliders' density/friction/restitution/sensor live in the nested
    // ColliderProperties sub-struct, so their offsets are a sum of two.
    physics::SphereCollider sc{};
    CHECK(field_set_float(&sc, *field_of("SphereCollider", "radius"), 2.5f), "radius set failed");
    CHECK(sc.radius == 2.5f, "sphere radius offset/type drift");
    CHECK(field_set_float3(&sc, *field_of("SphereCollider", "center"), Float3{9, 8, 7}),
          "center set failed");
    CHECK(sc.center.x == 9.0f, "sphere center offset/type drift");
    CHECK(field_set_float(&sc, *field_of("SphereCollider", "density"), 3.5f), "density set failed");
    CHECK(sc.properties.density == 3.5f, "sphere density (nested) offset drift");
    CHECK(field_set_float(&sc, *field_of("SphereCollider", "friction"), 0.25f), "friction set failed");
    CHECK(sc.properties.friction == 0.25f, "sphere friction (nested) offset drift");
    CHECK(field_set_float(&sc, *field_of("SphereCollider", "restitution"), 0.75f),
          "restitution set failed");
    CHECK(sc.properties.restitution == 0.75f, "sphere restitution (nested) offset drift");
    CHECK(field_set_bool(&sc, *field_of("SphereCollider", "sensor"), true), "sensor set failed");
    CHECK(sc.properties.sensor == true, "sphere sensor (nested) offset drift");

    physics::CapsuleCollider cc{};
    CHECK(field_set_float3(&cc, *field_of("CapsuleCollider", "point_a"), Float3{1, 1, 1}),
          "point_a set failed");
    CHECK(cc.point_a.y == 1.0f, "capsule point_a offset drift");
    CHECK(field_set_float3(&cc, *field_of("CapsuleCollider", "point_b"), Float3{2, 2, 2}),
          "point_b set failed");
    CHECK(cc.point_b.z == 2.0f, "capsule point_b offset drift");
    CHECK(field_set_float(&cc, *field_of("CapsuleCollider", "restitution"), 0.5f),
          "capsule restitution set failed");
    CHECK(cc.properties.restitution == 0.5f, "capsule restitution (nested) offset drift");

    physics::BoxCollider bc{};
    CHECK(field_set_float3(&bc, *field_of("BoxCollider", "half_extents"), Float3{3, 4, 5}),
          "half_extents set failed");
    CHECK(bc.half_extents.y == 4.0f, "box half_extents offset drift");
    CHECK(field_set_quat(&bc, *field_of("BoxCollider", "rotation"),
                         Quaternion{0.5f, 0.5f, 0.5f, 0.5f}), "box rotation set failed");
    CHECK(bc.rotation.w == 0.5f, "box rotation offset/type drift");
    CHECK(field_set_bool(&bc, *field_of("BoxCollider", "sensor"), true), "box sensor set failed");
    CHECK(bc.properties.sensor == true, "box sensor (nested) offset drift");

    physics::ConvexHullCollider hull{};
    hull.point_count = 12;
    uint32_t u = 0;
    CHECK(field_get_uint(&hull, *field_of("ConvexHullCollider", "point_count"), u) && u == 12,
          "hull point_count offset/type drift");
    CHECK(field_set_float(&hull, *field_of("ConvexHullCollider", "friction"), 0.9f),
          "hull friction set failed");
    CHECK(hull.properties.friction == 0.9f, "hull friction (nested) offset drift");
}

static void test_part_instance_offsets() {
    PartInstance pi{};
    pi.part_hash = 0x1122334455667788ull;
    const FieldDescriptor* hash_fd = field_of("PartInstance", "part_hash");
    CHECK(hash_fd->storage_size == sizeof(uint64_t), "part_hash storage width drift");
    uint32_t u = 0;
    CHECK(field_get_uint(&pi, *hash_fd, u), "part_hash get failed");
    CHECK(u == 0x55667788u, "part_hash should truncate to its low 32 bits");

    CHECK(field_set_bool(&pi, *field_of("PartInstance", "visible"), false), "visible set failed");
    CHECK(pi.visible == false, "visible offset/type drift");
    CHECK(field_set_bool(&pi, *field_of("PartInstance", "casts_shadow"), false),
          "casts_shadow set failed");
    CHECK(pi.casts_shadow == false, "casts_shadow offset/type drift");
}

static void test_read_only_fields_reject_writes() {
    PartInstance pi{};
    pi.part_hash = 0xABCDull;
    const FieldDescriptor* hash_fd = field_of("PartInstance", "part_hash");
    CHECK((hash_fd->flags & FieldReadOnly) != 0, "part_hash should be read-only");
    CHECK(!field_set_uint(&pi, *hash_fd, 7), "part_hash write should be rejected");
    CHECK(pi.part_hash == 0xABCDull, "rejected write must not modify the field");

    physics::ConvexHullCollider hull{};
    hull.point_count = 5;
    const FieldDescriptor* count_fd = field_of("ConvexHullCollider", "point_count");
    CHECK((count_fd->flags & FieldReadOnly) != 0, "point_count should be read-only");
    CHECK(!field_set_uint(&hull, *count_fd, 9), "point_count write should be rejected");
    CHECK(hull.point_count == 5, "rejected write must not modify the field");
}

static void test_accessors_reject_null_and_wrong_type() {
    physics::RigidBody rb{};
    float f = 0.0f;
    CHECK(!field_get_float(nullptr, *field_of("RigidBody", "gravity_scale"), f),
          "null instance should fail");
    CHECK(!field_set_float(nullptr, *field_of("RigidBody", "gravity_scale"), 1.0f),
          "null instance should fail");
    CHECK(!field_get_float(&rb, *field_of("RigidBody", "enable_sleep"), f),
          "float read of a bool field should fail");
    bool b = false;
    CHECK(!field_get_bool(&rb, *field_of("RigidBody", "gravity_scale"), b),
          "bool read of a float field should fail");
    uint32_t u = 0;
    CHECK(!field_get_uint(&rb, *field_of("RigidBody", "type"), u),
          "uint read of an enum field should fail");
}

static void test_to_props_desc() {
    matter::props::Desc d;
    const FieldDescriptor* gravity = field_of("RigidBody", "gravity_scale");
    CHECK(to_props_desc(*gravity, d), "float field should convert");
    CHECK(d.type == matter::props::Type::Float, "float type mapping");
    CHECK(d.offset == gravity->offset, "offset must survive the conversion");
    CHECK(d.has_range && d.min == -10.0f && d.max == 10.0f, "range must survive");

    // Quaternion is ECS-only by design (spec S7).
    CHECK(!to_props_desc(*field_of("LocalTransform", "rotation"), d),
          "Quaternion fields must not convert");
    // props' typed accessors are 32-bit; these two are not.
    CHECK(!to_props_desc(*field_of("RigidBody", "type"), d),
          "1-byte enum storage must not convert");
    CHECK(!to_props_desc(*field_of("PartInstance", "part_hash"), d),
          "8-byte uint storage must not convert");

    const FieldDescriptor* count = field_of("ConvexHullCollider", "point_count");
    CHECK(to_props_desc(*count, d), "4-byte uint field should convert");
    CHECK(d.type == matter::props::Type::UInt, "uint type mapping");
    CHECK((d.flags & matter::props::ReadOnly) != 0, "FieldReadOnly maps to props::ReadOnly");

    CHECK(to_props_desc(*field_of("LocalTransform", "translation"), d), "float3 should convert");
    CHECK(d.type == matter::props::Type::Float3, "float3 type mapping");
    CHECK(to_props_desc(*field_of("RigidBody", "enable_sleep"), d), "bool should convert");
    CHECK(d.type == matter::props::Type::Bool, "bool type mapping");
}

// ---------------------------------------------------------------------------
// Single-recipe validation tests.
// ---------------------------------------------------------------------------

static void test_validate_empty_id_rejected() {
    RawEntityRecipe raw;
    raw.authored_id = "";
    raw.components_json = "{}";

    EntityRecipe out;
    RecipeError err;
    CHECK(!validate(raw, out, err), "empty id should be rejected");
    CHECK(err.message.find("empty") != std::string::npos, "error should mention empty");
}

static void test_validate_unknown_component_rejected() {
    RawEntityRecipe raw;
    raw.authored_id = "entity_1";
    raw.components_json = R"({"UnknownWidget": {}})";

    EntityRecipe out;
    RecipeError err;
    CHECK(!validate(raw, out, err), "unknown component should be rejected");
    CHECK(err.message.find("unknown component") != std::string::npos, "error should mention unknown");
    CHECK(err.field_path == "UnknownWidget", "field_path should be the unknown key");
}

static void test_validate_multiple_colliders_rejected() {
    RawEntityRecipe raw;
    raw.authored_id = "entity_1";
    raw.components_json = R"({"SphereCollider": {}, "BoxCollider": {}})";

    EntityRecipe out;
    RecipeError err;
    CHECK(!validate(raw, out, err), "multiple colliders should be rejected");
    CHECK(err.message.find("multiple colliders") != std::string::npos, "error should mention multiple colliders");
}

static void test_validate_valid_recipe() {
    RawEntityRecipe raw;
    raw.authored_id = "ball_1";
    raw.display_name = "Ball";
    raw.components_json = R"({"RigidBody": {}, "SphereCollider": {}})";

    EntityRecipe out;
    RecipeError err;
    CHECK(validate(raw, out, err), "valid recipe should pass");
    CHECK(out.authored_id == "ball_1", "authored_id preserved");
}

static void test_validate_empty_components() {
    RawEntityRecipe raw;
    raw.authored_id = "empty_entity";
    raw.components_json = "{}";

    EntityRecipe out;
    RecipeError err;
    CHECK(validate(raw, out, err), "empty components should pass");
}

// ---------------------------------------------------------------------------
// Batch validation tests.
// ---------------------------------------------------------------------------

static void test_batch_duplicate_ids_rejected() {
    std::vector<RawEntityRecipe> recipes = {
        {"dup_id", "First", "", "{}"},
        {"dup_id", "Second", "", "{}"},
    };

    std::vector<EntityRecipe> out;
    RecipeError err;
    CHECK(!validate_batch(recipes, out, err), "duplicate ids should be rejected");
    CHECK(err.message.find("duplicate") != std::string::npos, "error should mention duplicate");
}

static void test_batch_missing_parent_rejected() {
    std::vector<RawEntityRecipe> recipes = {
        {"child_1", "Child", "nonexistent_parent", "{}"},
    };

    std::vector<EntityRecipe> out;
    RecipeError err;
    CHECK(!validate_batch(recipes, out, err), "missing parent should be rejected");
    CHECK(err.message.find("missing parent") != std::string::npos, "error should mention missing parent");
}

static void test_batch_cycle_rejected() {
    std::vector<RawEntityRecipe> recipes = {
        {"a", "A", "b", "{}"},
        {"b", "B", "c", "{}"},
        {"c", "C", "a", "{}"},
    };

    std::vector<EntityRecipe> out;
    RecipeError err;
    CHECK(!validate_batch(recipes, out, err), "cycle should be rejected");
    CHECK(err.message.find("cycle") != std::string::npos, "error should mention cycle");
}

static void test_batch_valid_hierarchy() {
    std::vector<RawEntityRecipe> recipes = {
        {"root", "Root", "", "{}"},
        {"child", "Child", "root", "{}"},
        {"grandchild", "GrandChild", "child", "{}"},
    };

    std::vector<EntityRecipe> out;
    RecipeError err;
    CHECK(validate_batch(recipes, out, err), "valid hierarchy should pass");
    CHECK(out.size() == 3, "should have 3 validated recipes");
}

// ---------------------------------------------------------------------------
// Instantiation tests.
// ---------------------------------------------------------------------------

static void test_instantiate_creates_entities() {
    flecs::world world;
    world.import<ecs::CoreModule>();
    world.import<physics::PhysicsModule>();
    world.import<streaming::StreamingModule>();
    world.import<SceneModule>();

    std::vector<EntityRecipe> recipes = {
        {"hero", "Hero", "", R"({"RigidBody": {}, "SphereCollider": {}})"},
        {"ground", "Ground", "", R"({"BoxCollider": {}})"},
    };

    SceneGeneration gen;
    RecipeError err;
    CHECK(instantiate(world, recipes.data(), (uint32_t)recipes.size(), gen, err),
          "instantiate should succeed");
    CHECK(gen.value == 1, "generation should increment");

    int scene_entities = 0;
    world.each([&](flecs::entity, const SceneEntityId&) { ++scene_entities; });
    CHECK(scene_entities == 2, "should create 2 entities");
}

static void test_instantiate_wires_parent() {
    flecs::world world;
    world.import<ecs::CoreModule>();
    world.import<physics::PhysicsModule>();
    world.import<streaming::StreamingModule>();
    world.import<SceneModule>();

    std::vector<EntityRecipe> recipes = {
        {"parent_e", "Parent", "", "{}"},
        {"child_e", "Child", "parent_e", "{}"},
    };

    SceneGeneration gen;
    RecipeError err;
    CHECK(instantiate(world, recipes.data(), (uint32_t)recipes.size(), gen, err),
          "instantiate with hierarchy should succeed");

    flecs::entity child;
    world.each([&](flecs::entity e, const SceneEntityId&) {
        if (e.parent().is_valid() && e.parent().has<SceneEntityId>())
            child = e;
    });
    CHECK(child.is_valid(), "child entity should exist");
    CHECK(child.parent().is_valid(), "child should have parent");
    CHECK(child.parent().has<SceneEntityId>(), "parent should have SceneEntityId");
}

static void test_instantiate_adds_components() {
    flecs::world world;
    world.import<ecs::CoreModule>();
    world.import<physics::PhysicsModule>();
    world.import<streaming::StreamingModule>();
    world.import<SceneModule>();

    std::vector<EntityRecipe> recipes = {
        {"phys_ball", "Ball", "", R"({"RigidBody": {}, "SphereCollider": {}, "PhysicsVelocity": {}})"},
    };

    SceneGeneration gen;
    RecipeError err;
    CHECK(instantiate(world, recipes.data(), (uint32_t)recipes.size(), gen, err),
          "instantiate with components should succeed");

    flecs::entity ball;
    world.each([&](flecs::entity e, const SceneEntityId&) { ball = e; });
    CHECK(ball.is_valid(), "ball entity should exist");
    CHECK(ball.has<physics::RigidBody>(), "should have RigidBody");
    CHECK(ball.has<physics::SphereCollider>(), "should have SphereCollider");
    CHECK(ball.has<physics::PhysicsVelocity>(), "should have PhysicsVelocity");
    CHECK(ball.has<ecs::LocalTransform>(), "should have LocalTransform");
}

static void test_instantiate_empty_is_noop() {
    flecs::world world;
    world.import<ecs::CoreModule>();
    world.import<physics::PhysicsModule>();
    world.import<streaming::StreamingModule>();
    world.import<SceneModule>();

    SceneGeneration gen;
    RecipeError err;
    CHECK(instantiate(world, nullptr, 0, gen, err), "empty instantiate should succeed");
    CHECK(gen.value == 0, "generation should not increment on empty");
}

// ---------------------------------------------------------------------------
// Identity tests.
// ---------------------------------------------------------------------------

static void test_authored_ids_produce_stable_hashes() {
    flecs::world world;
    world.import<ecs::CoreModule>();
    world.import<physics::PhysicsModule>();
    world.import<streaming::StreamingModule>();
    world.import<SceneModule>();

    std::vector<EntityRecipe> batch = {{"stable_id", "Test", "", "{}"}};
    SceneGeneration gen;
    RecipeError err;
    CHECK(instantiate(world, batch.data(), 1, gen, err), "first instantiate");

    uint64_t hash1 = 0;
    world.each([&](flecs::entity, const SceneEntityId& id) { hash1 = id.value; });

    flecs::world world2;
    world2.import<ecs::CoreModule>();
    world2.import<physics::PhysicsModule>();
    world2.import<streaming::StreamingModule>();
    world2.import<SceneModule>();

    SceneGeneration gen2;
    RecipeError err2;
    CHECK(instantiate(world2, batch.data(), 1, gen2, err2), "second instantiate");

    uint64_t hash2 = 0;
    world2.each([&](flecs::entity, const SceneEntityId& id) { hash2 = id.value; });

    CHECK(hash1 == hash2, "same authored_id should produce same hash");
    CHECK(hash1 != 0, "hash should be non-zero");
}

// ---------------------------------------------------------------------------
// Main.
// ---------------------------------------------------------------------------

int main() {
    test_scene_module_registers_scene_entity_id();
    test_scene_module_registers_part_instance();
    test_scene_module_registers_part_instance_error();
    test_core_module_reflects_transform();
    test_physics_module_reflects_rigid_body();
    test_physics_module_reflects_velocity();
    test_physics_module_reflects_sphere_collider();
    test_physics_module_reflects_capsule_collider();
    test_physics_module_reflects_box_collider();
    test_physics_module_reflects_convex_hull_collider();
    test_streaming_module_reflects_sector_streaming();

    test_find_component_known();
    test_find_component_unknown();
    test_component_count();

    test_find_field();
    test_component_struct_sizes_match();
    test_every_field_offset_is_in_bounds();
    test_transform_offsets();
    test_rigid_body_offsets();
    test_rigid_body_type_labels();
    test_velocity_offsets();
    test_collider_offsets();
    test_part_instance_offsets();
    test_read_only_fields_reject_writes();
    test_accessors_reject_null_and_wrong_type();
    test_to_props_desc();

    test_validate_empty_id_rejected();
    test_validate_unknown_component_rejected();
    test_validate_multiple_colliders_rejected();
    test_validate_valid_recipe();
    test_validate_empty_components();

    test_batch_duplicate_ids_rejected();
    test_batch_missing_parent_rejected();
    test_batch_cycle_rejected();
    test_batch_valid_hierarchy();

    test_instantiate_creates_entities();
    test_instantiate_wires_parent();
    test_instantiate_adds_components();
    test_instantiate_empty_is_noop();

    test_authored_ids_produce_stable_hashes();

    return check_summary();
}
