#include <matter/geometry/geometry.h>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>

namespace d = matter::contracts;
namespace s = matter::services;
using Vertex = std::array<long long, 3>;
void require(bool value, const std::string &message) {
    if (!value) throw std::runtime_error(message);
}
Vertex weld(d::Vec3 p) {
    return {std::llround(p.x * 100000), std::llround(p.y * 100000), std::llround(p.z * 100000)};
}
void mesh_check(const d::MeshPrototype &mesh) {
    require(!mesh.positions.empty() && !mesh.indices.empty() && mesh.indices.size() % 3 == 0, "empty or incomplete mesh");
    require(mesh.positions.size() < 200000 && mesh.indices.size() < 600000, "unbounded output");
    for (auto p : mesh.positions) {
        require(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z), "nonfinite position");
        require(std::abs(p.x) < 20 && std::abs(p.y) < 20 && std::abs(p.z) < 20, "position escaped fixture bounds");
        constexpr float e = 1e-4f;
        require(p.x >= mesh.bounds.min.x - e && p.x <= mesh.bounds.max.x + e &&
                p.y >= mesh.bounds.min.y - e && p.y <= mesh.bounds.max.y + e &&
                p.z >= mesh.bounds.min.z - e && p.z <= mesh.bounds.max.z + e, "incorrect prototype bounds");
    }
    // Geometry publishes triangle soup. Weld spatially at 10 micrometres to
    // test geometric edges, rather than rejecting intentional shading seams.
    // Every noncollapsed edge must have exactly two opposite incident faces.
    std::map<std::pair<Vertex, Vertex>, std::pair<unsigned, int>> edges;
    unsigned faces = 0;
    double volume = 0;
    for (size_t i = 0; i < mesh.indices.size(); i += 3) {
        std::array<Vertex, 3> face;
        std::array<d::Vec3, 3> points;
        for (size_t j = 0; j < 3; ++j) {
            require(mesh.indices[i + j] < mesh.positions.size(), "invalid index");
            points[j] = mesh.positions[mesh.indices[i + j]];
            face[j] = weld(points[j]);
        }
        if (face[0] == face[1] || face[1] == face[2] || face[2] == face[0]) continue;
        const auto cross = mm::cross(mm::sub(points[1], points[0]), mm::sub(points[2], points[0]));
        require(mm::dot(cross, cross) > 1e-16f, "zero-area face");
        volume += mm::dot(points[0], mm::cross(points[1], points[2])) / 6.0;
        ++faces;
        for (size_t j = 0; j < 3; ++j) {
            auto a = face[j], b = face[(j + 1) % 3];
            const int direction = a < b ? 1 : -1;
            if (b < a) std::swap(a, b);
            auto &edge = edges[{a, b}];
            ++edge.first;
            edge.second += direction;
        }
    }
    require(faces > 0 && volume > 1e-5, "empty, inverted or flat solid in " + mesh.id.value + " (volume " + std::to_string(volume) + ")");
    for (const auto &entry : edges)
        require(entry.second.first == 2 && entry.second.second == 0,
                "open, nonmanifold or inconsistently wound edge in " + mesh.id.value);
}
int main() {
    try {
        matter::geometry::PrimitiveGeometry geometry;
        d::ValidationOptions validation;
        validation.supported_capabilities = geometry.capabilities();
        s::EvaluationOptions options;
        options.validation = validation;
        options.budget.operations = 20000000;
        options.budget.vertices = 200000;
        options.budget.indices = 600000;
        options.budget.execution_microseconds = 2000000;
        std::string line;
        double total = 0;
        unsigned count = 0;
        while (std::getline(std::cin, line)) {
            const auto start = std::chrono::steady_clock::now();
            d::RecipeDocument recipe;
            const auto decoded = d::decode_recipe(line, recipe, validation);
            if (!decoded.ok()) {
                for (const auto &e : decoded.diagnostics) std::cerr << e.message << '\n';
                throw std::runtime_error("native recipe decode failed");
            }
            const auto result = geometry.evaluate({recipe.source.id.value, "1", 1}, recipe, {}, options, {});
            const auto evaluated = std::chrono::steady_clock::now();
            if (!result.ok()) {
                for (const auto &e : result.diagnostics()) std::cerr << recipe.source.id.value << ": " << e.message << '\n';
                throw std::runtime_error("native evaluation failed");
            }
            // The production service validates the published AssetProduct.
            // Independently check topology/bounds below without repeatedly
            // serializing and validating a copy for each shared placement.
            const auto &asset = result.value()->document;
            require(!asset.meshes.empty() && !asset.instances.empty(), "no geometry or placements");
            for (const auto &mesh : asset.meshes) mesh_check(mesh);
            for (const auto &instance : asset.instances) {
                bool found = false;
                for (const auto &mesh : asset.meshes) if (mesh.id == instance.prototype) {
                    for (auto local : mesh.positions) {
                        const auto p = mm::transform_point(instance.local_transform, local);
                        require(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) &&
                                std::abs(p.x) < 20 && std::abs(p.y) < 20 && std::abs(p.z) < 20, "unbounded placed output");
                        constexpr float e = 1e-4f;
                        require(p.x >= asset.bounds.min.x - e && p.x <= asset.bounds.max.x + e &&
                                p.y >= asset.bounds.min.y - e && p.y <= asset.bounds.max.y + e &&
                                p.z >= asset.bounds.min.z - e && p.z <= asset.bounds.max.z + e, "incorrect asset bounds");
                    }
                    found = true;
                }
                require(found, "missing instance prototype");
            }
            const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            total += elapsed;
            ++count;
            std::cout << recipe.source.id.value << " OK " << elapsed << "s (decode/eval "
                      << std::chrono::duration<double>(evaluated - start).count() << "s)\n";
        }
        require(count == 13, "expected all 13 recipes");
        require(total < 2, "catalog native evaluation and validation exceeded 2 seconds: " + std::to_string(total) + "s");
        std::cout << "13 recipes passed; native total " << total << "s\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
