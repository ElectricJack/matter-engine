#include <matter/geometry_compute/solid.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#ifdef MATTER_GEOMETRY_COMPUTE
#include <matter/solid_sdf_meshing.h>
#include "solid_spirv.h"
#endif

namespace matter::geometry_compute {
namespace {
#ifdef MATTER_GEOMETRY_COMPUTE
#include "mc_tables.h"
struct Failure { data::ValidationResult result; };
void checked(data::ValidationResult r) { if (!r.ok()) throw Failure{std::move(r)}; }
void require(bool ok, data::ErrorCode code, const char *message) {
    if (!ok) throw Failure{{{{data::Severity::error, code, message, {}, {}, {}}}}};
}
struct alignas(16) Params { float origin[4]{}, spacing[4]{}; uint32_t dims[4]{}, limits[4]{}; };
struct Vertex { float position[4], normal[4]; };
static_assert(sizeof(Params) == 64 && sizeof(Vertex) == 32, "canonical solid shader ABI");
std::vector<gpu_meshing::SolidOp> lower(const SolidRequest &request, jobs::JobMonitor &monitor) {
    require(!request.elements.empty() && request.elements.size() <= 256,
        data::ErrorCode::resource_limit, "solid request requires 1..256 elements");
    std::vector<gpu_meshing::SolidOp> ops;
    ops.reserve(request.elements.size());
    for (const auto &e : request.elements) {
        checked(monitor.checkpoint());
        require(e.shape >= Shape::box && e.shape <= Shape::capsule && e.combine >= Combine::unite && e.combine <= Combine::intersect,
            data::ErrorCode::unsupported_capability, "unknown solid shape/combine");
        checked(data::validate_transform(e.local_to_world, true));
        const auto &m = e.local_to_world;
        for (int a = 0; a < 3; ++a) for (int b = 0; b < 3; ++b) {
            float dot = 0; for (int k = 0; k < 3; ++k) dot += m(k,a) * m(k,b);
            require(std::abs(dot - (a == b ? 1.f : 0.f)) <= 1e-4f,
                data::ErrorCode::unsupported_capability, "solid compute requires rigid transforms; use physical dimensions");
        }
        float det = m(0,0)*(m(1,1)*m(2,2)-m(1,2)*m(2,1)) -
            m(0,1)*(m(1,0)*m(2,2)-m(1,2)*m(2,0)) + m(0,2)*(m(1,0)*m(2,1)-m(1,1)*m(2,0));
        require(det > 0, data::ErrorCode::unsupported_capability, "reflected solid compute transform unsupported");
        require(e.shape == Shape::rounded_box || e.rounding == 0,
            data::ErrorCode::invalid_input, "rounding is only supported for rounded boxes");
        data::Mat4 inverse;
        require(mm::inverse(m, inverse), data::ErrorCode::invalid_input, "singular solid transform");
        gpu_meshing::SolidOp op;
        for (int i = 0; i < 4; ++i) { op.row0[i] = inverse(0,i); op.row1[i] = inverse(1,i); op.row2[i] = inverse(2,i); }
        op.shape = {e.dimensions.x,e.dimensions.y,e.dimensions.z,e.rounding};
        op.kind = {static_cast<uint32_t>(e.shape),static_cast<uint32_t>(e.combine),0,0};
        op.blend = {e.smoothing,0,0,0}; ops.push_back(op);
    }
    return ops;
}
void initialize(compute::Buffer &buffer, const void *bytes, size_t size) {
    buffer.initial.resize(size); std::memcpy(buffer.initial.data(), bytes, size);
}
template<size_t N> compute::Dispatch dispatch(const uint32_t (&words)[N], uint32_t groups) {
    return {std::vector<uint32_t>(words, words + N),groups};
}
#endif
}
SolidBackend::SolidBackend(std::shared_ptr<compute::Device> device) : device_(std::move(device)) {}
std::vector<data::Capability> SolidBackend::capabilities() const {
#ifdef MATTER_GEOMETRY_COMPUTE
    if (device_) return {{"matter.geometry.solid.compute",1},{"matter.asset.core",1},{"matter.material.pbr",1}};
#endif
    return {};
}
jobs::AssetResult SolidBackend::generate(const jobs::JobEnvelope &job, const SolidRequest &request,
    const jobs::EvaluationOptions &options, const jobs::JobControl &control) {
    try {
        const auto admitted = job;
        auto validation = jobs::validate(admitted);
        if (validation.ok()) validation = jobs::validate(options.budget);
        if (!validation.ok()) return jobs::AssetResult::failure(admitted, std::move(validation.diagnostics));
        jobs::JobMonitor monitor(options.budget, control);
        validation = monitor.checkpoint();
        if (!validation.ok()) return jobs::AssetResult::failure(admitted, std::move(validation.diagnostics));
        if (request.version != 1) return jobs::AssetResult::failure(admitted,
            data::ErrorCode::unsupported_version, "unsupported solid compute request version");
        if (capabilities().empty()) return jobs::AssetResult::failure(admitted,
            data::ErrorCode::unsupported_capability, "solid compute backend disabled or no compute device supplied");
        validation = jobs::require_capabilities(request.required_capabilities, capabilities());
        if (validation.ok()) validation = jobs::require_capabilities(options.algorithms, capabilities());
        if (!validation.ok()) return jobs::AssetResult::failure(admitted, std::move(validation.diagnostics));
#ifdef MATTER_GEOMETRY_COMPUTE
        require(request.device == device_->id(), data::ErrorCode::invalid_input, "solid compute device identity mismatch");
        data::AssetDocument asset;
        asset.id = request.id; asset.provenance = request.source;
        asset.required_capabilities = {{"matter.asset.core",1},{"matter.material.pbr",1}};
        asset.materials = {request.material};
        auto bounded = options; bounded.validation.supported_capabilities = capabilities();
        checked(data::validate(asset, bounded.validation));
        checked(monitor.report({0,"lower"}));
        auto ops = lower(request, monitor);
        const auto capacity = std::min({options.budget.vertices, options.budget.indices,
            options.budget.output_bytes / 28, uint64_t(2 * 1024 * 1024)});
        require(capacity != 0, data::ErrorCode::resource_limit, "solid output budget is too small");
        gpu_meshing::SolidJob legacy;
        legacy.ops = ops.data(); legacy.op_count = static_cast<uint32_t>(ops.size());
        legacy.voxel_m = request.spacing_metres; legacy.max_grid_vertices = request.max_grid_vertices;
        legacy.max_mesh_vertices = static_cast<uint32_t>(capacity);
        gpu_meshing::GridLayout grid; gpu_meshing::Error error;
        if (!gpu_meshing::validate_solid_job(legacy, grid, error))
            return jobs::AssetResult::failure(admitted, error.code == gpu_meshing::ErrorCode::LimitExceeded ?
                data::ErrorCode::resource_limit : data::ErrorCode::invalid_input, error.message);
        const auto vertices = std::min(legacy.max_mesh_vertices, grid.grid_cells * 15u);
        jobs::ResourceUsage usage;
        // Conservative field operations, including six field samples per normal.
        usage.operations = uint64_t(grid.grid_vertices)*ops.size() + uint64_t(grid.grid_cells)*8 +
            uint64_t(vertices)*(6*ops.size()+3);
        const uint64_t sizes[] = {sizeof(Params), ops.size()*sizeof(gpu_meshing::SolidOp),
            uint64_t(grid.grid_vertices)*4, uint64_t(grid.grid_cells)*8,
            ((uint64_t(grid.grid_cells)+255)/256)*4, 256*16*4, 16, uint64_t(vertices)*sizeof(Vertex)};
        uint64_t device_bytes = 0; for (auto size : sizes) device_bytes += size;
        // Explicit buffers, readback, final mesh, and validation headroom. Driver
        // private pipeline allocations are not measurable through this budget.
        const uint64_t host_bytes = sizes[7] + uint64_t(vertices)*128 + sizes[1] + sizes[5] + 65536;
        require(host_bytes < options.budget.working_bytes && device_bytes <= options.budget.working_bytes-host_bytes,
            data::ErrorCode::resource_limit, "solid working memory budget exceeded");
        usage.working_bytes = host_bytes + device_bytes;
        checked(monitor.checkpoint(usage));
        compute::Request compute;
        compute.device = request.device; compute.allocation_limit = options.budget.working_bytes-host_bytes;
        compute.buffers.resize(8);
        for (size_t i = 0; i < 8; ++i) compute.buffers[i].size = sizes[i];
        compute.buffers[6].readback = compute.buffers[7].readback = true;
        Params params;
        params.origin[0] = grid.origin_m.x; params.origin[1] = grid.origin_m.y; params.origin[2] = grid.origin_m.z;
        params.spacing[0] = params.spacing[1] = params.spacing[2] = request.spacing_metres;
        params.spacing[3] = request.spacing_metres*.02f;
        for (int i = 0; i < 3; ++i) params.dims[i] = grid.sample_dims[i];
        params.dims[3] = grid.grid_vertices;
        params.limits[0] = legacy.op_count; params.limits[1] = grid.grid_cells; params.limits[2] = vertices;
        int32_t triangles[256*16];
        for (int i = 0; i < 256; ++i) for (int k = 0; k < 16; ++k)
            triangles[i*16+k] = static_cast<signed char>(triTable[i][k]);
        initialize(compute.buffers[0], &params, sizeof(params));
        initialize(compute.buffers[1], ops.data(), static_cast<size_t>(sizes[1]));
        initialize(compute.buffers[5], triangles, sizeof(triangles));
        compute.dispatches = {dispatch(spirv::solid_field,(grid.grid_vertices+255)/256),
            dispatch(spirv::solid_classify,(grid.grid_cells+255)/256),dispatch(spirv::solid_scan,1),
            dispatch(spirv::solid_emit,(grid.grid_cells+255)/256)};
        checked(monitor.report({0.2,"dispatch"}));
        auto result = device_->execute(admitted,compute,monitor);
        if (!result.ok()) return jobs::AssetResult::failure(admitted,result.diagnostics());
        require(result.envelope() == admitted, data::ErrorCode::stale_result, "compute result envelope mismatch");
        checked(monitor.checkpoint());
        const auto &buffers = result.value()->buffers;
        require(result.value()->allocated_bytes <= compute.allocation_limit,
            data::ErrorCode::backend_failure, "compute allocation receipt exceeds admitted limit");
        require(buffers.size() == 8 && buffers[6].size() == 16 && buffers[7].size() == sizes[7],
            data::ErrorCode::backend_failure, "malformed compute readback");
        uint32_t status[4]; std::memcpy(status,buffers[6].data(),sizeof(status));
        require(!status[1] && status[0] <= vertices, data::ErrorCode::resource_limit,
            "solid output capacity exceeded; no partial mesh published");
        require(status[0]%3 == 0, data::ErrorCode::backend_failure, "invalid compute triangle count");
        usage.vertices = usage.indices = status[0]; usage.output_bytes = uint64_t(status[0])*28;
        usage.working_bytes = host_bytes + result.value()->allocated_bytes;
        checked(monitor.checkpoint(usage));
        if (status[0]) {
            data::MeshPrototype mesh; mesh.id = {request.id.value+".mesh"};
            mesh.positions.reserve(status[0]); mesh.normals.reserve(status[0]); mesh.indices.reserve(status[0]);
            for (uint32_t i = 0; i < status[0]; i += 3) {
                if ((i&1023u) == 0) checked(monitor.checkpoint());
                Vertex triangle[3];
                std::memcpy(triangle,buffers[7].data()+size_t(i)*sizeof(Vertex),sizeof(triangle));
                for (const auto &v : triangle) for (int k = 0; k < 3; ++k)
                    require(std::isfinite(v.position[k]) && std::isfinite(v.normal[k]),
                        data::ErrorCode::backend_failure, "nonfinite solid output");
                const auto *p = triangle[0].position, *q = triangle[1].position, *r = triangle[2].position;
                const double ux=double(q[0])-p[0], uy=double(q[1])-p[1], uz=double(q[2])-p[2];
                const double vx=double(r[0])-p[0], vy=double(r[1])-p[1], vz=double(r[2])-p[2];
                const double x=uy*vz-uz*vy, y=uz*vx-ux*vz, z=ux*vy-uy*vx;
                // Grid-aligned isosurfaces can collapse table triangles. Remove
                // exactly zero area, using the contract's double cross product.
                if (x*x+y*y+z*z == 0) continue;
                for (const auto &v : triangle) {
                    data::Vec3 point{v.position[0],v.position[1],v.position[2]};
                    if (mesh.positions.empty()) mesh.bounds = {point,point};
                    mesh.indices.push_back(static_cast<uint32_t>(mesh.positions.size()));
                    mesh.positions.push_back(point); mesh.normals.push_back({v.normal[0],v.normal[1],v.normal[2]});
                    mesh.bounds.min = {std::min(mesh.bounds.min.x,point.x),std::min(mesh.bounds.min.y,point.y),std::min(mesh.bounds.min.z,point.z)};
                    mesh.bounds.max = {std::max(mesh.bounds.max.x,point.x),std::max(mesh.bounds.max.y,point.y),std::max(mesh.bounds.max.z,point.z)};
                }
            }
            if (!mesh.positions.empty()) {
                mesh.submeshes.push_back({0,static_cast<uint32_t>(mesh.indices.size()),request.material.id});
                asset.bounds = mesh.bounds;
                asset.instances.push_back({request.id,mesh.id,data::InstanceTarget::mesh,{}, {}});
                asset.meshes.push_back(std::move(mesh));
            }
        }
        checked(monitor.report({1,"complete"}));
        return jobs::finish_asset(admitted,std::move(asset),bounded,monitor,usage);
#else
        return jobs::AssetResult::failure(admitted,data::ErrorCode::unsupported_capability,"solid compute not compiled");
#endif
#ifdef MATTER_GEOMETRY_COMPUTE
    } catch (const Failure &f) { return jobs::AssetResult::failure(job,f.result.diagnostics);
#endif
    } catch (const std::bad_alloc &) { return jobs::AssetResult::failure(job,data::ErrorCode::resource_limit,"solid allocation failed");
    } catch (const std::exception &e) { return jobs::AssetResult::failure(job,data::ErrorCode::backend_failure,e.what());
    } catch (...) { return jobs::AssetResult::failure(job,data::ErrorCode::backend_failure,"solid backend exception"); }
}
} // namespace matter::geometry_compute
