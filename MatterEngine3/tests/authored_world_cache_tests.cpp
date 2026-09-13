#include "authored_world_cache.h"
#include <cstdio>
#include <limits>
#include <string>
using namespace authored_world_cache;
static int failures = 0;
#define CHECK(x)                                                                       \
    do {                                                                               \
        if (!(x)) {                                                                    \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                  \
            ++failures;                                                                \
        }                                                                              \
    } while (0)
static void resign(std::string& b) {
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i + 8 < b.size(); i++) {
        h ^= static_cast<unsigned char>(b[i]);
        h *= 1099511628211ull;
    }
    for (int i = 0; i < 8; i++)
        b[b.size() - 8 + i] = char(h >> (8 * i));
}
int main() {
    std::puts("authored_world_cache_tests: static definition and ordered materials");
    MaterialRegistryResetDynamic();
    MaterialDef material{};
    MaterialRegistryDefaultDynamicDef(&material);
    material.surfaceFlags = 32;
    material.transmission = .21f;
    material.absorptionColor[1] = .37f;
    material.specularTint[2] = .61f;
    const int id = MaterialRegistryDefineDynamic(&material, "finished-brick");
    CHECK(id == MaterialRegistryStaticCount());
    matter::WorldDefinition w;
    matter::WorldRoot root;
    root.module = "CastleSiteSurfaceMasonry";
    root.params_json = "{\"material\":" + std::to_string(id) + "}";
    root.transform.m[3] = 17;
    root.expand = true;
    w.roots.push_back(root);
    w.materials.push_back({"finished-brick", id, "CastleStoneDetail", 410});
    w.entities.push_back({"door", "Walkable doorway", "",
                          "{\"BoxCollider\":{\"halfExtents\":[1,2,0.2]}}"});
    matter::WorldPropSpec prop;
    prop.name = "density";
    prop.has_range = true;
    prop.min = .125f;
    prop.max = 7.25f;
    prop.number_default = 2;
    w.props.push_back(prop);
    w.settings.camera.authored = true;
    w.settings.camera.position = {1, 2, 3};
    w.settings.camera.target = {4, 5, 6};
    w.settings.sun_angular_diameter_deg = .77f;
    w.settings.atmosphere.mie_scale = 1.7f;
    w.settings.cloud_shadows.filter_scale = 2.3f;
    w.settings.fog.clouds[3].weather_influence = .43f;
    w.settings.volumetrics.powder_strength = .31f;
    w.settings.streaming_rings.push_back({32, 2});
    w.settings.terrain_bands.push_back({64, 1});
    matter::WorldLight light;
    light.kind = matter::WorldLightKind::Spot;
    light.intensity = 45;
    light.casts_shadow = true;
    w.lights.push_back(light);
    std::string blob;
    CHECK(capture(w, blob));
    Snapshot decoded;
    CHECK(decode(blob, decoded));
    CHECK(decoded.world.entities[0].components_json == w.entities[0].components_json);
    CHECK(decoded.world.props[0] == prop);
    CHECK(decoded.world.settings.camera.position.x == 1);
    CHECK(decoded.world.settings.fog.clouds[3].weather_influence == .43f);
    CHECK(decoded.world.settings.cloud_shadows.filter_scale == 2.3f);
    CHECK(decoded.world.settings.volumetrics.powder_strength == .31f);
    CHECK(decoded.world.settings.terrain_bands[0].radius == 64);
    CHECK(decoded.world.materials[0].detail_density == 410);
    CHECK(decoded.world.materials[0].detail_module == "CastleStoneDetail");
    CHECK(decoded.materials[0].definition.surfaceFlags == 32);
    MaterialRegistryResetDynamic();
    CHECK(replay_materials(decoded));
    CHECK(MaterialRegistryFindByName("finished-brick") == id);
    CHECK(MaterialRegistryGet(id)->groundTilesetSlot == -1);
    CHECK(MaterialRegistryGet(id)->absorptionColor[1] == .37f);
    std::string roundtrip;
    CHECK(capture(decoded.world, roundtrip));
    CHECK(roundtrip == blob);
    std::puts("authored_world_cache_tests: corruption, schema, limits and fallback");
    for (size_t n : {size_t(0), size_t(12), blob.size() - 1})
        CHECK(!decode(blob.substr(0, n), decoded));
    auto bad = blob;
    bad[25] ^= 1;
    CHECK(!decode(bad, decoded));
    bad = blob;
    bad[4] = 99;
    resign(bad);
    CHECK(!decode(bad, decoded));
    bad = blob;
    for (int i = 0; i < 4; i++)
        bad[12 + i] = char(255);
    resign(bad);
    CHECK(!decode(bad, decoded));
    bad = blob;
    bad.insert(bad.end() - 8, 'x');
    resign(bad);
    CHECK(!decode(bad, decoded));
    CHECK(decode(blob, decoded));
    decoded.materials[0].index++;
    CHECK(!replay_materials(decoded));
    CHECK(MaterialRegistryFindByName("finished-brick") == id);
    CHECK(decode(blob, decoded));
    Material second = decoded.materials[0];
    second.index++;
    second.name = std::string(4096, 'x');
    decoded.materials.push_back(second);
    CHECK(!replay_materials(decoded));
    CHECK(MaterialRegistryDynamicCount() == 0); // first insertion rolled back
    CHECK(decode(blob, decoded));
    CHECK(replay_materials(decoded));
    MaterialDef stale = material;
    stale.roughness = .1f;
    MaterialRegistryResetDynamic();
    CHECK(MaterialRegistryDefineDynamic(&stale, "stale-world") == id);
    CHECK(replay_materials(decoded));
    CHECK(MaterialRegistryFindByName("stale-world") == -1);
    CHECK(MaterialRegistryFindByName("finished-brick") == id);
    w.hydrology.emplace();
    CHECK(!capture(w, bad));
    w.hydrology.reset();
    w.river_network.emplace();
    CHECK(!capture(w, bad));
    w.river_network.reset();
    w.terrain_collision.emplace();
    CHECK(!capture(w, bad));
    w.terrain_collision.reset();
    w.settings.sector_size = std::numeric_limits<float>::quiet_NaN();
    CHECK(!capture(w, bad));
    MaterialRegistryResetDynamic();
    std::printf("authored_world_cache_tests: %s (%d failures)\n",
                failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
