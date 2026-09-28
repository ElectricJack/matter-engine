#pragma once

// CPU-only preparation and its bounded worker. No Vulkan object or borrowed
// request output/pool pointer crosses this boundary. GPU publication stays on
// the recorder thread. The queue uses the engine's existing channel transport.
#include "matter/event/channel.h"
#include "vt_snapshot.h"
#include "vt_chart_gpu.h"
#include "vt_surface_tape.h"
#include "vt_periodic_material.h"
#include "vt_surface_boundary.h"
#include "vt_seed_bvh.h"

#include <atomic>
#include <functional>
#include <limits>
#include <map>
#include <thread>

namespace vt {

using VtPreparedCorners = std::shared_ptr<const std::vector<VtTriangleCorners>>;

struct VtPreparedInputs {
    std::vector<GpuChart> charts;
    std::vector<GpuTriGeometry> geometry;
    std::vector<VtSeedNode> seed_nodes;
    VtPreparedCorners corners;
    std::shared_ptr<const VtSurfaceBoundary> boundary;
    std::vector<GpuTriSurface> weights, lanes;
    std::vector<uint32_t> finite_ids;
    VtSurfaceTapePack tape;
    enum class Tape { Absent, Ready, Failed, LaneOverflow } tape_state = Tape::Absent;
};

inline size_t vt_preparation_triangle_bound(const chart_atlas::ChartAtlasRung& atlas) {
    size_t count = 0;
    for (const auto& chart : atlas.charts) {
        const size_t first = std::min(size_t(chart.first_tri), atlas.tri_order.size());
        const size_t n = std::min(size_t(chart.tri_count), atlas.tri_order.size() - first);
        if (n > std::numeric_limits<size_t>::max() - count) return SIZE_MAX;
        count += n;
    }
    return std::max(count, atlas.tri_order.size());
}

inline bool vt_prepare_cpu(const chart_atlas::ChartAtlasRung& atlas,
    const VtPartContext& ctx, VtPreparedCorners reuse, bool tape_gpu,
    VtPreparedInputs& out) {
    out = {};
    const bool has_tape = vt_context_has_tape(ctx);
    if(ctx.periodic.version && (!vt_valid_periodic_domain(ctx.periodic) ||
        atlas.atlas_w!=ctx.periodic.width || atlas.atlas_h!=ctx.periodic.height ||
        atlas.charts.size()!=1 || ctx.vertex_count!=4 || ctx.triangle_count!=2 ||
        !has_tape || !ctx.surface_tape_text || ctx.surface_material_count!=1 ||
        ctx.surface_lane_count || ctx.surface_world_anchored)) return false;
    if (bool(ctx.finite_sources)!=bool(ctx.finite_source_ids)) return false;
    if (has_tape && ctx.surface_tape_text) {
        terrain_field::SurfaceProgram program;
        std::string error;
        if (!terrain_field::SurfaceProgram::parse(ctx.surface_tape_text, program, error)) {
            out.tape_state = VtPreparedInputs::Tape::Failed;
            // A malformed source can fail before its source directive is
            // reached. Its partially parsed version cannot authorize fallback.
            return false;
        } else if (!tape_gpu) {
            // A direct source has no equivalent vertex-weight fallback. Keep
            // existing complete pages instead of publishing a different material.
            if (program.source.version != 0) return false;
        } else if (!vt_pack_surface_tape(program, ctx.surface_world_anchored != 0, out.tape)) {
            out.tape_state = out.tape.lane_overflow
                ? VtPreparedInputs::Tape::LaneOverflow : VtPreparedInputs::Tape::Failed;
        } else if (out.tape.weight_reg_count != ctx.surface_material_count ||
                   out.tape.scan.count != ctx.surface_lane_count ||
                   (out.tape.scan.count && !ctx.surface_lanes)) {
            out.tape_state = VtPreparedInputs::Tape::Failed;
        } else {
            out.tape_state = VtPreparedInputs::Tape::Ready;
        }
        if (program.source.version != 0 && out.tape_state != VtPreparedInputs::Tape::Ready)
            return false;
        if(ctx.periodic.version && (program.source.version!=1 || program.uses_world_inputs())) return false;
    }
    if (reuse) {
        out.corners = std::move(reuse);
    } else {
        std::vector<GpuTri> combined;
        auto corners = std::make_shared<std::vector<VtTriangleCorners>>();
        // Overlapping chart ranges can repeat triangles. Reserve the full
        // bound so the builder never grows by an unaccounted capacity factor.
        const size_t bound = vt_preparation_triangle_bound(atlas);
        combined.reserve(bound);
        corners->reserve(bound);
        auto geometry_ctx = ctx;
        geometry_ctx.surface_weights = nullptr;
        geometry_ctx.surface_material_count = 0;
        if (!vt_build_chart_gpu_streams(atlas, geometry_ctx, out.charts, combined,
                                        nullptr, 0, corners.get())) return false;
        if (!vt_build_chart_surface_neighbors(geometry_ctx, *corners, combined)) return false;
        out.geometry.resize(combined.size());
        for (size_t i = 0; i < combined.size(); ++i)
            std::memcpy(&out.geometry[i], &combined[i], sizeof(GpuTriGeometry));
        auto boundary=std::make_shared<VtSurfaceBoundary>();
        if(!vt_extract_surface_boundary(out.charts,out.geometry,*boundary))return false;
        out.boundary=std::move(boundary);
        if (!vt_build_seed_bvh(out.charts, out.geometry, out.seed_nodes)) return false;
        out.corners = std::move(corners);
    }
    out.weights.resize(out.corners->size());
    if (ctx.finite_sources) {
        // Finite images have no vertex-weight substitute. A coherent direct
        // base supplies mortar/holes and the final composed-height contract.
        if (out.tape_state!=VtPreparedInputs::Tape::Ready || out.tape.source.version!=1) return false;
        const auto& receivers=ctx.finite_sources->receivers.empty()?ctx.finite_sources->bindings:ctx.finite_sources->receivers;
        out.finite_ids.resize(out.corners->size());
        for (size_t i=0;i<out.corners->size();++i) {
            const auto &c=(*out.corners)[i];
            const uint32_t id=ctx.finite_source_ids[c[0]];
            if (id>receivers.size() || id!=ctx.finite_source_ids[c[1]] || id!=ctx.finite_source_ids[c[2]]) return false;
            out.finite_ids[i]=id;
            if (!id) continue;
            const auto &binding=receivers[id-1];
            for (uint32_t vertex:c) {
                const float *p=ctx.positions+size_t(vertex)*3;
                float depth=0;
                for (int k=0;k<3;++k) depth+=(p[k]-binding.origin_datum[k])*binding.n[k];
                if (!std::isfinite(depth)||std::abs(depth-binding.origin_datum[3])>binding.u[3]) return false;
            }
        }
    }
    const bool mode3 = out.tape_state == VtPreparedInputs::Tape::Ready;
    if (mode3) out.lanes.resize(out.corners->size());
    const uint32_t columns = has_tape ? ctx.surface_material_count : 0;
    for (size_t i = 0; i < out.corners->size(); ++i) {
        const auto* corners = (*out.corners)[i].data();
        out.weights[i] = vt_pack_triangle_surface(ctx, corners, columns, nullptr, 0);
        if (mode3) out.lanes[i] = vt_pack_triangle_surface(ctx, corners, columns,
                                                        ctx.surface_lanes, ctx.surface_lane_count);
    }
    return true;
}

class VtCpuPreparer {
public:
    struct Limits { size_t jobs = 32, bytes = 256u * 1024u * 1024u; };
    struct Stats {
        uint64_t submitted = 0, completed = 0, cancelled = 0, failed = 0;
        uint64_t deferred = 0, oversized = 0;
        size_t retained_jobs = 0, reserved_bytes = 0, peak_bytes = 0;
    };
    using Builder = std::function<bool(const VtPartSnapshot&, VtPreparedCorners,
                                      bool, VtPreparedInputs&)>;
    VtCpuPreparer() : VtCpuPreparer(Limits{}) {}
    explicit VtCpuPreparer(Limits limits, Builder builder = {})
        : limits_(limits), queue_({limits.jobs, matter::evt::OnFull::RejectNewest}),
          budget_(std::make_shared<Budget>()), builder_(std::move(builder)) {}
    ~VtCpuPreparer() { shutdown(); }
    VtCpuPreparer(const VtCpuPreparer&) = delete;
    VtCpuPreparer& operator=(const VtCpuPreparer&) = delete;
    void begin_frame() { ++epoch_; admission_blocked_ = false; }

    // Recorder thread. Returns readiness only; never waits for worker output.
    bool prepare(const VtPreparationKey& key, std::shared_ptr<const VtPartSnapshot> input,
                 VtPreparedCorners reuse, bool tape_gpu) {
        if (stopped_ || !input) return false;
        auto found = jobs_.find(key);
        if (found != jobs_.end()) {
            if (found->second->input != input || found->second->reuse != reuse ||
                found->second->tape_gpu != tape_gpu) {
                cancel(key);
            } else {
                found->second->last_requested_epoch = epoch_;
                const State state = found->second->state.load(std::memory_order_acquire);
                if (state == State::Ready) return true;
                if (state == State::Failed) {
                    jobs_.erase(found);
                    admission_blocked_ = false;
                    ++stats_.failed;
                }
                ++stats_.deferred;
                return false;
            }
        }
        if (admission_blocked_) { ++stats_.deferred; return false; }
        const size_t bytes = estimate(*input, reuse, limits_.bytes);
        if (bytes > limits_.bytes) { ++stats_.oversized; return false; }
        // Completed work that lost demand must not permanently occupy the
        // queue's byte/job capacity. Protect every result admitted this frame
        // until fill consumes it; older unused results are expendable.
        while (budget_->jobs.load(std::memory_order_relaxed) >= limits_.jobs ||
               bytes > limits_.bytes - std::min(budget_->bytes.load(std::memory_order_relaxed), limits_.bytes)) {
            auto victim = jobs_.end();
            for (auto it = jobs_.begin(); it != jobs_.end(); ++it) {
                if (it->second.use_count() != 1 ||
                    it->second->last_requested_epoch >= epoch_ ||
                    it->second->state.load(std::memory_order_acquire) == State::Pending) continue;
                if (victim == jobs_.end() ||
                    it->second->last_requested_epoch < victim->second->last_requested_epoch) victim = it;
            }
            if (victim == jobs_.end()) break;
            cancel(victim->first);
        }
        const size_t current = budget_->bytes.load(std::memory_order_relaxed);
        if (!limits_.jobs || budget_->jobs.load(std::memory_order_relaxed) >= limits_.jobs ||
            bytes > limits_.bytes - std::min(current, limits_.bytes)) {
            admission_blocked_ = true;
            ++stats_.deferred;
            return false;
        }
        try {
            if (!worker_.joinable()) worker_ = std::thread([this] { work(); });
            auto job = std::make_shared<Job>(budget_, bytes);
            job->input = std::move(input);
            job->reuse = std::move(reuse);
            job->tape_gpu = tape_gpu;
            job->last_requested_epoch = epoch_;
            jobs_.emplace(key, job);
            if (queue_.push(job) != matter::evt::PushResult::Queued) {
                jobs_.erase(key);
                ++stats_.deferred;
                return false;
            }
            ++stats_.submitted;
            stats_.peak_bytes = std::max(stats_.peak_bytes, current + bytes);
        } catch (const std::exception&) {
            jobs_.erase(key);
            ++stats_.failed;
        }
        return false;
    }

    // Valid until consume/cancel. The release/acquire state handoff is the
    // only writer/reader transition; ready results are never mutated again.
    const VtPreparedInputs* ready(const VtPreparationKey& key,
                                  const std::shared_ptr<const VtPartSnapshot>& input) const {
        const auto found = jobs_.find(key);
        if (found == jobs_.end() || found->second->input != input ||
            found->second->state.load(std::memory_order_acquire) != State::Ready) return nullptr;
        return &found->second->output;
    }
    // A partial GPU upload pins the result AND its job/byte reservation.
    // Erasing/cancelling the queue entry cannot refund storage still in use.
    std::shared_ptr<const VtPreparedInputs> retain_ready(const VtPreparationKey& key,
        const std::shared_ptr<const VtPartSnapshot>& input) const {
        const auto* output = ready(key, input);
        if (!output) return {};
        return {jobs_.find(key)->second, output};
    }
    void consume(const VtPreparationKey& key) {
        if (jobs_.erase(key)) { ++stats_.completed; admission_blocked_ = false; }
    }
    void cancel(const VtPreparationKey& key) {
        const auto found = jobs_.find(key);
        if (found == jobs_.end()) return;
        found->second->cancelled.store(true, std::memory_order_relaxed);
        jobs_.erase(found);
        admission_blocked_ = false;
        ++stats_.cancelled;
    }
    void cancel_part(uint64_t hash) {
        for (auto it = jobs_.begin(); it != jobs_.end();) {
            const auto key = (it++)->first;
            if (key.variant_hash == hash) cancel(key);
        }
    }
    Stats stats() const {
        auto result = stats_;
        result.retained_jobs = budget_->jobs.load(std::memory_order_relaxed);
        result.reserved_bytes = budget_->bytes.load(std::memory_order_relaxed);
        return result;
    }
    void shutdown() {
        if (stopped_) return;
        stopped_ = true;
        for (auto& item : jobs_) item.second->cancelled.store(true, std::memory_order_relaxed);
        jobs_.clear();
        queue_.shut_down();
        if (worker_.joinable()) worker_.join(); // teardown only, never frame recording
    }

private:
    struct Budget { std::atomic<size_t> jobs{0}, bytes{0}; };
    enum class State { Pending, Ready, Failed };
    struct Job {
        std::shared_ptr<Budget> budget;
        size_t bytes;
        std::shared_ptr<const VtPartSnapshot> input;
        VtPreparedCorners reuse;
        bool tape_gpu = false;
        uint64_t last_requested_epoch = 0; // recorder thread only
        std::atomic<bool> cancelled{false};
        std::atomic<State> state{State::Pending};
        VtPreparedInputs output;
        Job(std::shared_ptr<Budget> b, size_t n) : budget(std::move(b)), bytes(n) {
            budget->jobs.fetch_add(1, std::memory_order_relaxed);
            budget->bytes.fetch_add(bytes, std::memory_order_relaxed);
        }
        ~Job() {
            // Free payload BEFORE returning its byte reservation to admission.
            output = {}; reuse.reset(); input.reset();
            budget->bytes.fetch_sub(bytes, std::memory_order_relaxed);
            budget->jobs.fetch_sub(1, std::memory_order_relaxed);
        }
    };
    // Conservative capacity accounting, including retained input snapshots,
    // both surface modes, transient combined triangles, and parser headroom.
    // Shared inputs/corners are deliberately counted per job (never undercounted).
    static size_t estimate(const VtPartSnapshot& snapshot, const VtPreparedCorners& reuse,
                           size_t limit) {
        size_t bytes = 0;
        bool overflow = false;
        const auto add = [&](size_t count, size_t stride) {
            if (overflow || count > (limit - bytes) / stride) { overflow = true; return; }
            bytes += count * stride;
        };
        const auto vector = [&](const auto& data) { add(data.capacity(), sizeof(data[0])); };
        const auto& g = *snapshot.geometry;
        const auto& s = *snapshot.surface;
        add(1, sizeof(Job) + sizeof(VtPartSnapshot) + sizeof(VtGeometryInputs) + sizeof(VtSurfaceInputs));
        vector(g.atlas.charts); vector(g.atlas.tri_order);
        vector(g.positions); vector(g.normals); vector(g.surface_uvs); vector(g.material_table);
        vector(g.material_ids); vector(g.indices); vector(g.tint_rgba);
        vector(s.weights); vector(s.materials); vector(s.lanes);
        vector(s.finite_source_ids);
        if (s.finite_sources) add(1,s.finite_sources->bytes());
        add(s.tape_text.capacity() + 1, 8); // retained text + parser temporaries
        add(1, 64u * 1024u); // strings, queue/control-block overhead
        // Parsing can grow its op vector geometrically. Charge its capacity,
        // packed GPU ops and register-allocation scratch at the new source cap.
        add(terrain_field::kMaxSurfaceSourceOps,
            2 * sizeof(terrain_field::Op) + sizeof(VtGpuSurfOp) + 2 * sizeof(int));
        const size_t triangles = reuse ? reuse->size() : vt_preparation_triangle_bound(g.atlas);
        if (reuse) vector(*reuse);
        else {
            add(g.atlas.charts.size(), sizeof(GpuChart));
            add(triangles, sizeof(GpuTri) + sizeof(GpuTriGeometry) + sizeof(VtTriangleCorners));
            add(triangles / 2 + triangles % 2, sizeof(VtSeedNode));
            add(triangles, 3*sizeof(VtSurfaceBoundaryEdge));
            // Exact-position welding and oriented edge maps are transient.
            // Charge six hash nodes/buckets per triangle, allocator headroom,
            // emitted indices and adjacency; appearance-only jobs skip this.
            add(triangles, 1024);
        }
        add(triangles, 2 * sizeof(GpuTriSurface));
        if (s.finite_sources) add(triangles,sizeof(uint32_t));
        return overflow ? SIZE_MAX : bytes;
    }
    void work() {
        std::shared_ptr<Job> job;
        while (queue_.wait_pop(job) == matter::evt::WaitResult::Item) {
            if (!job->cancelled.load(std::memory_order_relaxed)) {
                bool ok = false;
                try {
                    ok = builder_ ? builder_(*job->input, job->reuse, job->tape_gpu, job->output)
                        : vt_prepare_cpu(*job->input->context.atlas, job->input->context,
                                         job->reuse, job->tape_gpu, job->output);
                } catch (const std::exception&) { ok = false; }
                job->state.store(ok ? State::Ready : State::Failed, std::memory_order_release);
            }
            job.reset(); // don't retain the last job while sleeping on an empty queue
        }
    }
    Limits limits_;
    matter::evt::Channel<std::shared_ptr<Job>> queue_;
    std::shared_ptr<Budget> budget_;
    Builder builder_;
    std::thread worker_;
    std::map<VtPreparationKey, std::shared_ptr<Job>> jobs_;
    Stats stats_;
    uint64_t epoch_ = 0;
    bool admission_blocked_ = false;
    bool stopped_ = false;
};

} // namespace vt
