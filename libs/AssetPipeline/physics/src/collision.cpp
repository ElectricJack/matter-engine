#include <matter/asset/collision.h>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace matter::asset::collision {
namespace {
using V = data::Value;
using O = V::Object;
using A = V::Array;
data::ValidationResult error(data::ErrorCode c, std::string m) {
    return {{{data::Severity::error, c, std::move(m), {}, {}, {}}}};
}
bool finite(data::Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
bool pose_ok(const Pose& p) {
    const auto q = p.rotation;
    const double n = double(q.x)*q.x + double(q.y)*q.y + double(q.z)*q.z + double(q.w)*q.w;
    return finite(p.position) && std::isfinite(n) && std::abs(n - 1) <= 1e-4;
}
V vec(data::Vec3 p) { return A{p.x, p.y, p.z}; }
V pose(const Pose& p) { return O{{"position", vec(p.position)}, {"rotation", A{p.rotation.x,p.rotation.y,p.rotation.z,p.rotation.w}}}; }
const O& object(const V& v, std::initializer_list<const char*> keys) {
    const auto* o = std::get_if<O>(&v.data);
    if (!o || o->size() != keys.size()) throw std::runtime_error("incorrect object fields");
    for (auto k : keys) if (!o->count(k)) throw std::runtime_error("missing field: " + std::string(k));
    return *o;
}
const A& array(const V& v) {
    const auto* a = std::get_if<A>(&v.data);
    if (!a) throw std::runtime_error("expected array");
    return *a;
}
std::string string(const V& v) {
    const auto* s = std::get_if<std::string>(&v.data);
    if (!s) throw std::runtime_error("expected string");
    return *s;
}
float number(const V& v) {
    const auto* n = std::get_if<double>(&v.data);
    if (!n || !std::isfinite(*n) || std::abs(*n) > std::numeric_limits<float>::max()) throw std::runtime_error("expected finite float32");
    return static_cast<float>(*n);
}
uint32_t integer(const V& v) {
    const auto* n = std::get_if<double>(&v.data);
    if (!n || !std::isfinite(*n) || *n < 0 || *n > UINT32_MAX || *n != std::floor(*n)) throw std::runtime_error("expected uint32");
    return static_cast<uint32_t>(*n);
}
bool boolean(const V& v) {
    const auto* b = std::get_if<bool>(&v.data);
    if (!b) throw std::runtime_error("expected boolean");
    return *b;
}
data::Vec3 read_vec(const V& v) {
    const auto& a = array(v);
    if (a.size()!=3) throw std::runtime_error("expected xyz");
    return {number(a[0]),number(a[1]),number(a[2])};
}
Pose read_pose(const V& v) {
    const auto& o = object(v, {"position", "rotation"});
    const auto& a = array(o.at("rotation"));
    if (a.size()!=4) throw std::runtime_error("expected xyzw");
    return {read_vec(o.at("position")), {number(a[0]),number(a[1]),number(a[2]),number(a[3])}};
}
} // namespace

data::ValidationResult validate(const Product& p, const data::ValidationLimits& l) {
    using E = data::ErrorCode;
    if (p.version != 1) return error(E::unsupported_version, "collision product version");
    auto caps = services::require_capabilities(p.required_capabilities, {{capability, 1}});
    if (!caps.ok()) return caps;
    if (p.shapes.size() > l.max_nodes || p.bodies.size() > l.max_instances || p.joints.size() > l.max_nodes)
        return error(E::resource_limit, "collision object budget");
    std::set<std::string> shapes, bodies, joints;
    auto id_ok = [&](const data::SemanticId& id, std::set<std::string>& ids) {
        return !id.value.empty() && id.value.size() <= l.max_string_bytes && ids.insert(id.value).second;
    };
    uint64_t vertices = 0, indices = 0;
    for (const auto& s : p.shapes) {
        if (!id_ok(s.id, shapes) || !pose_ok(s.local)) return error(E::invalid_input, "invalid shape identity or local pose");
        if (s.kind != "sphere" && s.kind != "capsule" && s.kind != "box" && s.kind != "convex_hull" && s.kind != "triangle_mesh")
            return error(E::unsupported_capability, "unsupported collision shape: " + s.kind);
        if (!finite(s.half_extents) || !std::isfinite(s.radius) || !std::isfinite(s.half_segment) ||
            s.radius <= 0 || s.half_segment < 0 || s.half_extents.x <= 0 || s.half_extents.y <= 0 || s.half_extents.z <= 0)
            return error(E::invalid_input, "invalid collision dimensions");
        vertices += s.vertices.size(); indices += s.indices.size();
        if (vertices > l.max_vertices || indices > l.max_indices) return error(E::resource_limit, "collision mesh budget");
        for (auto v : s.vertices) if (!finite(v)) return error(E::invalid_input, "nonfinite collision vertex");
        if (s.kind == "triangle_mesh") {
            if (s.indices.size()%3 || (s.vertices.empty() != s.indices.empty())) return error(E::invalid_input, "incomplete triangle mesh");
            for (auto i : s.indices) if (i >= s.vertices.size()) return error(E::invalid_input, "collision index out of range");
            for (size_t i=0;i<s.indices.size();i+=3) {
                auto a=s.vertices[s.indices[i]], b=s.vertices[s.indices[i+1]], c=s.vertices[s.indices[i+2]];
                double ux=double(b.x)-a.x, uy=double(b.y)-a.y, uz=double(b.z)-a.z;
                double vx=double(c.x)-a.x, vy=double(c.y)-a.y, vz=double(c.z)-a.z;
                if (uy*vz-uz*vy==0 && uz*vx-ux*vz==0 && ux*vy-uy*vx==0) return error(E::invalid_input, "degenerate collision triangle");
            }
        } else if (!s.indices.empty() || (s.kind != "convex_hull" && !s.vertices.empty())) return error(E::invalid_input, "unexpected shape geometry");
        if (s.kind == "convex_hull" && s.vertices.size() < 4) return error(E::invalid_input, "convex hull requires four points");
    }
    for (const auto& b : p.bodies) {
        if (!id_ok(b.id, bodies) || !shapes.count(b.shape.value) || !pose_ok(b.pose)) return error(E::invalid_input, "invalid body identity, shape reference or pose");
        if (b.motion != "static" && b.motion != "kinematic" && b.motion != "dynamic") return error(E::unsupported_capability, "unsupported body motion");
        if (!std::isfinite(b.density) || b.density<=0 || !std::isfinite(b.friction) || b.friction<0 || !std::isfinite(b.restitution) || b.restitution<0 || b.restitution>1)
            return error(E::invalid_input, "invalid collision material");
    }
    for (const auto& j : p.joints) {
        if (!id_ok(j.id, joints) || !bodies.count(j.body_a.value) || !bodies.count(j.body_b.value) || j.body_a==j.body_b ||
            !pose_ok(j.frame_a) || !pose_ok(j.frame_b) || !std::isfinite(j.distance) || j.distance<0)
            return error(E::invalid_input, "invalid joint identity, references or frames");
        if (j.kind != "distance" && j.kind != "weld") return error(E::unsupported_capability, "unsupported joint: " + j.kind);
        if (j.kind == "weld" && j.distance != 0) return error(E::invalid_input, "weld joint has a distance");
    }
    if (!std::holds_alternative<O>(p.generation.data)) return error(E::invalid_input, "generation provenance must be an object");
    // Enforce aggregate strings/bytes/depth as well as geometry counts.
    return data::validate_value(to_value(p), l);
}

data::Value to_value(const Product& p) {
    A shapes, bodies, joints, caps;
    for (const auto& c : p.required_capabilities) caps.emplace_back(O{{"name",c.name},{"version",c.version}});
    for (const auto& s : p.shapes) {
        A vertices, indices;
        for (auto v : s.vertices) vertices.push_back(vec(v));
        for (auto i : s.indices) indices.emplace_back(i);
        shapes.emplace_back(O{{"id",s.id.value},{"kind",s.kind},{"local",pose(s.local)}, {"half_extents",vec(s.half_extents)},
            {"radius",s.radius},{"half_segment",s.half_segment},{"vertices",std::move(vertices)},{"indices",std::move(indices)}});
    }
    for (const auto& b : p.bodies) bodies.emplace_back(O{{"id",b.id.value},{"shape",b.shape.value},{"pose",pose(b.pose)},
        {"motion",b.motion},{"density",b.density},{"friction",b.friction},{"restitution",b.restitution}});
    for (const auto& j : p.joints) joints.emplace_back(O{{"id",j.id.value},{"body_a",j.body_a.value},{"body_b",j.body_b.value},{"kind",j.kind},
        {"frame_a",pose(j.frame_a)},{"frame_b",pose(j.frame_b)},{"distance",j.distance},{"collide_connected",j.collide_connected}});
    return O{{"version",p.version},{"required_capabilities",std::move(caps)},{"shapes",std::move(shapes)},
        {"bodies",std::move(bodies)},{"joints",std::move(joints)},{"generation",p.generation}};
}
data::ValidationResult encode(const Product& p, std::string& out, const data::ValidationLimits& l) {
    auto v=validate(p,l); if (!v.ok()) return v;
    return data::canonical_json(to_value(p),out,l);
}
data::ValidationResult decode(std::string_view bytes, Product& out, const data::ValidationLimits& l) {
    V v; auto valid=data::parse_json(bytes,v,l); if (!valid.ok()) return valid;
    try {
        const auto& o=object(v,{"version","required_capabilities","shapes","bodies","joints","generation"});
        Product p; p.version=integer(o.at("version")); p.generation=o.at("generation");
        for (const auto& x:array(o.at("required_capabilities"))) { const auto& c=object(x,{"name","version"}); p.required_capabilities.push_back({string(c.at("name")),integer(c.at("version"))}); }
        for (const auto& x:array(o.at("shapes"))) {
            const auto& s=object(x,{"id","kind","local","half_extents","radius","half_segment","vertices","indices"});
            Shape shape; shape.id={string(s.at("id"))}; shape.kind=string(s.at("kind")); shape.local=read_pose(s.at("local"));
            shape.half_extents=read_vec(s.at("half_extents")); shape.radius=number(s.at("radius")); shape.half_segment=number(s.at("half_segment"));
            for (const auto& point:array(s.at("vertices"))) shape.vertices.push_back(read_vec(point));
            for (const auto& index:array(s.at("indices"))) shape.indices.push_back(integer(index));
            p.shapes.push_back(std::move(shape));
        }
        for (const auto& x:array(o.at("bodies"))) {
            const auto& b=object(x,{"id","shape","pose","motion","density","friction","restitution"});
            p.bodies.push_back({{string(b.at("id"))},{string(b.at("shape"))},read_pose(b.at("pose")),string(b.at("motion")),number(b.at("density")),number(b.at("friction")),number(b.at("restitution"))});
        }
        for (const auto& x:array(o.at("joints"))) {
            const auto& j=object(x,{"id","body_a","body_b","kind","frame_a","frame_b","distance","collide_connected"});
            p.joints.push_back({{string(j.at("id"))},{string(j.at("body_a"))},{string(j.at("body_b"))},string(j.at("kind")),read_pose(j.at("frame_a")),read_pose(j.at("frame_b")),number(j.at("distance")),boolean(j.at("collide_connected"))});
        }
        valid=validate(p,l); if (valid.ok()) out=std::move(p); return valid;
    } catch (const std::exception& e) { return error(data::ErrorCode::invalid_input,e.what()); }
}
data::ExtensionEnvelope extension(const Product& p) { return {{capability,1},true,to_value(p)}; }
data::ValidationResult decode(const data::ExtensionEnvelope& e, Product& out, const data::ValidationLimits& l) {
    if (e.capability.name!=capability) return error(data::ErrorCode::unsupported_capability,"not a collision product extension");
    if (e.capability.version!=1) return error(data::ErrorCode::unsupported_version,"collision extension version");
    std::string bytes; auto v=data::canonical_json(e.payload,bytes,l); if (!v.ok()) return v;
    return decode(bytes,out,l);
}
data::ValidationResult convert_solver(std::string_view source, std::string_view destination) {
    return error(data::ErrorCode::unsupported_capability,"solver-state conversion is unsupported: " + std::string(source) + " -> " + std::string(destination));
}
} // namespace matter::asset::collision
