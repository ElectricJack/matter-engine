#include "tileset_settle.h"

#include <cmath>
#include <cstring>

#include "box3d/box3d.h"
#include "box3d/collision.h"
#include "box3d/math_functions.h"

namespace tileset {

// ---- small math helpers ----------------------------------------------------

static b3Quat to_b3quat(const Pose& p) {
    b3Quat q; q.v.x = p.qx; q.v.y = p.qy; q.v.z = p.qz; q.s = p.qw; return q;
}

// Column-axes rotation matrix (columns = collider axes) -> quaternion.
static void axes_to_quat(const float axis[3][3], float out_xyzw[4]) {
    // m[r][c], columns are axis[0], axis[1], axis[2] (right-handed enforced).
    float a2[3] = { axis[2][0], axis[2][1], axis[2][2] };
    float cx = axis[0][1] * axis[1][2] - axis[0][2] * axis[1][1];
    float cy = axis[0][2] * axis[1][0] - axis[0][0] * axis[1][2];
    float cz = axis[0][0] * axis[1][1] - axis[0][1] * axis[1][0];
    if (cx * a2[0] + cy * a2[1] + cz * a2[2] < 0.0f) { a2[0] = -a2[0]; a2[1] = -a2[1]; a2[2] = -a2[2]; }
    float m[3][3] = {
        { axis[0][0], axis[1][0], a2[0] },
        { axis[0][1], axis[1][1], a2[1] },
        { axis[0][2], axis[1][2], a2[2] },
    };
    float tr = m[0][0] + m[1][1] + m[2][2];
    float x, y, z, w;
    if (tr > 0.0f) {
        float s = std::sqrt(tr + 1.0f) * 2.0f;
        w = 0.25f * s; x = (m[2][1] - m[1][2]) / s; y = (m[0][2] - m[2][0]) / s; z = (m[1][0] - m[0][1]) / s;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        float s = std::sqrt(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2.0f;
        w = (m[2][1] - m[1][2]) / s; x = 0.25f * s; y = (m[0][1] + m[1][0]) / s; z = (m[0][2] + m[2][0]) / s;
    } else if (m[1][1] > m[2][2]) {
        float s = std::sqrt(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f;
        w = (m[0][2] - m[2][0]) / s; x = (m[0][1] + m[1][0]) / s; y = 0.25f * s; z = (m[1][2] + m[2][1]) / s;
    } else {
        float s = std::sqrt(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2.0f;
        w = (m[1][0] - m[0][1]) / s; x = (m[0][2] + m[2][0]) / s; y = (m[1][2] + m[2][1]) / s; z = 0.25f * s;
    }
    out_xyzw[0] = x; out_xyzw[1] = y; out_xyzw[2] = z; out_xyzw[3] = w;
}

// ---- impl -------------------------------------------------------------------

struct SettleWorld::Impl {
    b3WorldId world;
    float torus = 0.0f;        // sim units
    SettleParams params;
    // base mesh data must outlive the world's mesh shape
    std::vector<b3Vec3> base_verts;
    std::vector<int32_t> base_indices;
    b3MeshData* base_mesh = nullptr;

    struct TrackedBody { b3BodyId id; int group; int instance; };
    std::vector<TrackedBody> bodies;   // spawn order
    std::vector<Pose> out_poses;

    struct SyncGroup {
        std::vector<Pose> frames;                    // sim-scaled occurrence frames
        std::vector<std::vector<int>> members;       // [occurrence][canonical idx] -> bodies index
    };
    std::vector<SyncGroup> groups;

    void wrap_bodies();
    void sync_groups_step();   // Task 5
    void refresh_poses();
    int count_awake() const;
};

SettleWorld::SettleWorld(float torus_size, const HeightField& base, const SettleParams& params) {
    impl_ = new Impl();
    impl_->params = params;
    const float S = params.sim_scale;
    impl_->torus = torus_size * S;

    b3WorldDef wdef = b3DefaultWorldDef();
    wdef.gravity = (b3Vec3){ 0.0f, -9.8f * S, 0.0f };
    impl_->world = b3CreateWorld(&wdef);

    // Base terrain: two CCW-up triangles per heightfield cell.
    const int nx = base.count_x, nz = base.count_z;
    impl_->base_verts.reserve((size_t)nx * nz);
    for (int z = 0; z < nz; ++z)
        for (int x = 0; x < nx; ++x)
            impl_->base_verts.push_back((b3Vec3){
                x * base.cell * S,
                base.heights[(size_t)z * nx + x] * S,
                z * base.cell * S });
    for (int z = 0; z + 1 < nz; ++z)
        for (int x = 0; x + 1 < nx; ++x) {
            int32_t a = z * nx + x,       b = z * nx + x + 1;
            int32_t c = (z + 1) * nx + x, d = (z + 1) * nx + x + 1;
            // +Y-up winding (counter-clockwise seen from above).
            impl_->base_indices.insert(impl_->base_indices.end(), { a, c, b });
            impl_->base_indices.insert(impl_->base_indices.end(), { b, c, d });
        }
    b3MeshDef mdef;
    std::memset(&mdef, 0, sizeof mdef);   // plain struct: no Default fn / cookie
    mdef.vertices = impl_->base_verts.data();
    mdef.vertexCount = (int)impl_->base_verts.size();
    mdef.indices = impl_->base_indices.data();
    mdef.triangleCount = (int)(impl_->base_indices.size() / 3);
    mdef.identifyEdges = true;
    impl_->base_mesh = b3CreateMesh(&mdef, nullptr, 0);

    b3BodyDef gdef = b3DefaultBodyDef();
    gdef.type = b3_staticBody;
    b3BodyId ground = b3CreateBody(impl_->world, &gdef);
    b3ShapeDef gsdef = b3DefaultShapeDef();
    b3CreateMeshShape(ground, &gsdef, impl_->base_mesh, (b3Vec3){ 1.0f, 1.0f, 1.0f });
}

SettleWorld::~SettleWorld() {
    b3DestroyWorld(impl_->world);
    if (impl_->base_mesh) b3DestroyMesh(impl_->base_mesh);
    delete impl_;
}

int SettleWorld::add_sync_group(const std::vector<Pose>& occurrence_frames) {
    Impl::SyncGroup g;
    g.frames = occurrence_frames;
    for (Pose& f : g.frames) { f.px *= impl_->params.sim_scale; f.py *= impl_->params.sim_scale; f.pz *= impl_->params.sim_scale; }
    impl_->groups.push_back(std::move(g));
    return (int)impl_->groups.size() - 1;
}

LayerResult SettleWorld::settle_layer(const std::vector<BodySpawn>& spawns) {
    const float S = impl_->params.sim_scale;

    for (const BodySpawn& sp : spawns) {
        b3BodyDef bd = b3DefaultBodyDef();
        bd.type = b3_dynamicBody;
        bd.position = (b3Pos){ sp.start.px * S, sp.start.py * S, sp.start.pz * S };
        bd.rotation = to_b3quat(sp.start);
        bd.linearVelocity = (b3Vec3){ sp.vx * S, sp.vy * S, sp.vz * S };
        b3BodyId id = b3CreateBody(impl_->world, &bd);

        b3ShapeDef sd = b3DefaultShapeDef();
        // Mass must stay scale-invariant: density is per sim-volume, which is
        // volume * S^3, so divide density by S^3.
        sd.density = sp.density / (S * S * S);
        sd.baseMaterial.friction = sp.friction;

        const ColliderFit& c = *sp.collider;
        const float cx = c.center[0] * S, cy = c.center[1] * S, cz = c.center[2] * S;
        switch (c.type) {
        case ColliderType::Sphere: {
            b3Sphere s = { (b3Vec3){ cx, cy, cz }, c.radius * S };
            b3CreateSphereShape(id, &sd, &s);
            break;
        }
        case ColliderType::Capsule: {
            b3Capsule cap;
            cap.center1 = (b3Vec3){ cx - c.seg_half * S * c.axis[0][0],
                                    cy - c.seg_half * S * c.axis[0][1],
                                    cz - c.seg_half * S * c.axis[0][2] };
            cap.center2 = (b3Vec3){ cx + c.seg_half * S * c.axis[0][0],
                                    cy + c.seg_half * S * c.axis[0][1],
                                    cz + c.seg_half * S * c.axis[0][2] };
            cap.radius = c.radius * S;
            b3CreateCapsuleShape(id, &sd, &cap);
            break;
        }
        case ColliderType::Box: {
            float q[4];
            axes_to_quat(c.axis, q);
            b3Transform xf;
            xf.p = (b3Vec3){ cx, cy, cz };
            xf.q.v.x = q[0]; xf.q.v.y = q[1]; xf.q.v.z = q[2]; xf.q.s = q[3];
            b3BoxHull bh = b3MakeTransformedBoxHull(
                c.half_extent[0] * S, c.half_extent[1] * S, c.half_extent[2] * S, xf);
            b3CreateHullShape(id, &sd, &bh.base);
            break;
        }
        case ColliderType::Hull: {
            std::vector<b3Vec3> pts;
            for (size_t i = 0; i + 2 < c.hull_points.size(); i += 3)
                pts.push_back((b3Vec3){ c.hull_points[i] * S,
                                        c.hull_points[i+1] * S,
                                        c.hull_points[i+2] * S });
            b3HullData* hull = b3CreateHull(pts.data(), (int)pts.size(), 32);
            b3CreateHullShape(id, &sd, hull);
            b3DestroyHull(hull);
            break;
        }
        }
        impl_->bodies.push_back(Impl::TrackedBody{ id, sp.sync_group, sp.instance });
        if (sp.sync_group >= 0) {
            Impl::SyncGroup& g = impl_->groups[sp.sync_group];
            if ((int)g.members.size() <= sp.instance) g.members.resize(sp.instance + 1);
            // Canonical index within the group is the order of first-occurrence
            // spawns; members[occurrence] lists bodies in canonical order.
            g.members[sp.instance].push_back((int)impl_->bodies.size() - 1);
        }
    }

    LayerResult r;
    const SettleParams& P = impl_->params;
    const int total = (int)impl_->bodies.size();
    while (r.sim_time < P.max_sim_time) {
        b3World_Step(impl_->world, P.dt, P.substeps);
        impl_->wrap_bodies();
        impl_->sync_groups_step();
        r.sim_time += P.dt;
        r.awake_count = impl_->count_awake();
        if (r.awake_count <= (int)((1.0f - P.sleep_fraction) * total)) {
            r.converged = true;
            break;
        }
    }
    impl_->refresh_poses();
    return r;
}

void SettleWorld::finalize() {
    // Task 5: snap sync groups, kinematic micro-relax. Base version: no-op.
    impl_->refresh_poses();
}

const std::vector<Pose>& SettleWorld::poses() const { return impl_->out_poses; }

uint64_t SettleWorld::pose_hash() const {
    uint64_t h = 1469598103934665603ull;   // FNV-1a
    for (const Pose& p : impl_->out_poses) {
        unsigned char buf[sizeof(Pose)];
        std::memcpy(buf, &p, sizeof(Pose));
        for (unsigned char b : buf) { h ^= b; h *= 1099511628211ull; }
    }
    return h;
}

void SettleWorld::Impl::wrap_bodies() {
    if (torus <= 0.0f) return;
    for (const TrackedBody& tb : bodies) {
        if (!b3Body_IsAwake(tb.id)) continue;
        b3Pos p = b3Body_GetPosition(tb.id);
        float nx = p.x, nz = p.z;
        while (nx < 0.0f)     nx += torus;
        while (nx >= torus)   nx -= torus;
        while (nz < 0.0f)     nz += torus;
        while (nz >= torus)   nz -= torus;
        if (nx != p.x || nz != p.z)
            b3Body_SetTransform(tb.id, (b3Pos){ nx, p.y, nz }, b3Body_GetRotation(tb.id));
    }
}

void SettleWorld::Impl::sync_groups_step() {
    // Implemented in Task 5.
}

void SettleWorld::Impl::refresh_poses() {
    const float inv = 1.0f / params.sim_scale;
    out_poses.clear();
    out_poses.reserve(bodies.size());
    for (const TrackedBody& tb : bodies) {
        b3Pos p = b3Body_GetPosition(tb.id);
        b3Quat q = b3Body_GetRotation(tb.id);
        out_poses.push_back(Pose{ p.x * inv, p.y * inv, p.z * inv,
                                  q.v.x, q.v.y, q.v.z, q.s });
    }
}

int SettleWorld::Impl::count_awake() const {
    int n = 0;
    for (const TrackedBody& tb : bodies)
        if (b3Body_IsAwake(tb.id)) ++n;
    return n;
}

} // namespace tileset
