#include "animation/ozz_adapter.h"

#include "ozz/animation/offline/animation_builder.h"
#include "ozz/animation/offline/animation_optimizer.h"
#include "ozz/animation/offline/raw_animation.h"
#include "ozz/animation/offline/raw_skeleton.h"
#include "ozz/animation/offline/skeleton_builder.h"
#include "ozz/animation/runtime/animation.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wignored-attributes"
#include "ozz/animation/runtime/blending_job.h"
#pragma GCC diagnostic pop
#include "ozz/animation/runtime/ik_two_bone_job.h"
#include "ozz/animation/runtime/local_to_model_job.h"
#include "ozz/animation/runtime/sampling_job.h"
#include "ozz/animation/runtime/skeleton.h"
#include "ozz/base/io/archive.h"
#include "ozz/base/io/stream.h"
#include "ozz/base/maths/simd_math.h"
#include "ozz/base/maths/simd_quaternion.h"
#include "ozz/base/maths/soa_transform.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <unordered_map>

namespace matter::animation {
namespace {

class BufferStream final : public ozz::io::Stream {
public:
    BufferStream() = default;
    explicit BufferStream(const uint8_t* data, std::size_t size) : bytes_(data, data + size) {}
    bool opened() const override { return true; }
    std::size_t Read(void* data, std::size_t size) override {
        const std::size_t available = cursor_ <= bytes_.size() ? bytes_.size() - cursor_ : 0;
        const std::size_t count = std::min(size, available);
        if (count != 0) std::memcpy(data, bytes_.data() + cursor_, count);
        if (count != size) { std::memset(static_cast<uint8_t*>(data) + count, 0, size - count); failed_ = true; }
        cursor_ += size;
        return size;
    }
    std::size_t Write(const void* data, std::size_t size) override {
        if (cursor_ + size > bytes_.size()) bytes_.resize(cursor_ + size);
        if (size != 0) std::memcpy(bytes_.data() + cursor_, data, size);
        cursor_ += size;
        return size;
    }
    int Seek(int offset, Origin origin) override {
        const long long base = origin == kSet ? 0 : origin == kCurrent ? static_cast<long long>(cursor_) : static_cast<long long>(bytes_.size());
        const long long next = base + offset;
        if (next < 0 || static_cast<std::size_t>(next) > bytes_.size()) return -1;
        cursor_ = static_cast<std::size_t>(next);
        return 0;
    }
    int Tell() const override { return cursor_ <= static_cast<std::size_t>(std::numeric_limits<int>::max()) ? static_cast<int>(cursor_) : -1; }
    std::size_t Size() const override { return bytes_.size(); }
    const std::vector<uint8_t>& bytes() const { return bytes_; }
    void seek(std::size_t offset) { cursor_ = offset; }
    bool failed() const { return failed_; }
private:
    std::vector<uint8_t> bytes_;
    std::size_t cursor_ = 0;
    bool failed_ = false;
};

void append_u16(std::vector<uint8_t>& bytes, uint16_t value) { bytes.push_back(static_cast<uint8_t>(value)); bytes.push_back(static_cast<uint8_t>(value >> 8)); }
bool read_u16(const uint8_t* data, std::size_t size, std::size_t& offset, uint16_t& value) {
    if (offset + 2 > size) return false;
    value = static_cast<uint16_t>(data[offset]) | (static_cast<uint16_t>(data[offset + 1]) << 8);
    offset += 2;
    return true;
}

Quaternion multiply(Quaternion a, Quaternion b) {
    return {a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
            a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
            a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
            a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z};
}
Quaternion normalize(Quaternion q) {
    const float length = std::sqrt(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
    return length > 0.0f ? Quaternion{q.x/length, q.y/length, q.z/length, q.w/length} : Quaternion{};
}
void to_soa(const std::vector<AnimationTransform>& values, std::vector<ozz::math::SoaTransform>& output) {
    output.resize((values.size() + 3) / 4);
    for (std::size_t group = 0; group < output.size(); ++group) {
        ozz::math::SimdFloat4 translations[4];
        ozz::math::SimdFloat4 rotations[4];
        ozz::math::SimdFloat4 scales[4];
        for (std::size_t lane = 0; lane < 4; ++lane) {
            const std::size_t index = group * 4 + lane;
            const AnimationTransform identity{};
            const AnimationTransform& value = index < values.size() ? values[index] : identity;
            translations[lane] = ozz::math::simd_float4::Load3PtrU(&value.translation.x);
            rotations[lane] = ozz::math::simd_float4::LoadPtrU(&value.rotation.x);
            scales[lane] = ozz::math::simd_float4::Load3PtrU(&value.scale.x);
        }
        ozz::math::Transpose4x3(translations, &output[group].translation.x);
        ozz::math::Transpose4x4(rotations, &output[group].rotation.x);
        ozz::math::Transpose4x3(scales, &output[group].scale.x);
    }
}
void from_soa(const std::vector<ozz::math::SoaTransform>& values, std::size_t count, std::vector<AnimationTransform>& output) {
    output.resize(count);
    for (std::size_t group = 0; group < values.size(); ++group) {
        ozz::math::SimdFloat4 translations[4], rotations[4], scales[4];
        ozz::math::Transpose3x4(&values[group].translation.x, translations);
        ozz::math::Transpose4x4(&values[group].rotation.x, rotations);
        ozz::math::Transpose3x4(&values[group].scale.x, scales);
        for (std::size_t lane = 0; lane < 4 && group * 4 + lane < count; ++lane) {
            AnimationTransform& value = output[group * 4 + lane];
            ozz::math::Store3PtrU(translations[lane], &value.translation.x);
            ozz::math::StorePtrU(rotations[lane], &value.rotation.x);
            ozz::math::Store3PtrU(scales[lane], &value.scale.x);
        }
    }
}
ozz::math::Float4x4 to_ozz_matrix(const Mat4f& value) {
    ozz::math::Float4x4 output;
    for (int column = 0; column < 4; ++column) {
        const float values[4] = {value.m[column], value.m[4 + column], value.m[8 + column], value.m[12 + column]};
        output.cols[column] = ozz::math::simd_float4::LoadPtrU(values);
    }
    return output;
}
Mat4f from_ozz_matrix(const ozz::math::Float4x4& value) {
    Mat4f output{};
    for (int column = 0; column < 4; ++column) {
        float values[4];
        ozz::math::StorePtrU(value.cols[column], values);
        for (int row = 0; row < 4; ++row) output.m[row * 4 + column] = values[row];
    }
    return output;
}

std::vector<std::size_t> canonical_order(const RigDefinition& rig, Diagnostics& diagnostics) {
    std::unordered_map<std::string, std::size_t> index;
    for (std::size_t i=0; i<rig.joints.size(); ++i) index.emplace(rig.joints[i].name, i);
    std::vector<std::vector<std::size_t>> children(rig.joints.size());
    std::size_t root = rig.joints.size();
    for (std::size_t i=0; i<rig.joints.size(); ++i) {
        if (rig.joints[i].parent.empty()) { if (root == rig.joints.size()) root = i; else { diagnostics.add("ozz-skeleton", rig.joints[i].source, "rig has multiple roots"); return {}; } }
        else { const auto parent=index.find(rig.joints[i].parent); if (parent == index.end()) { diagnostics.add("ozz-skeleton", rig.joints[i].source, "rig parent is missing"); return {}; } children[parent->second].push_back(i); }
    }
    if (root == rig.joints.size()) { diagnostics.add("ozz-skeleton", rig.source, "rig has no root"); return {}; }
    std::vector<std::size_t> result;
    std::function<void(std::size_t)> visit = [&](std::size_t joint) { result.push_back(joint); for (std::size_t child : children[joint]) visit(child); };
    visit(root);
    if (result.size() != rig.joints.size()) { diagnostics.add("ozz-skeleton", rig.source, "rig hierarchy is cyclic"); return {}; }
    return result;
}

void build_raw_children(const RigDefinition& rig, const std::vector<std::vector<std::size_t>>& children, std::size_t index, ozz::animation::offline::RawSkeleton::Joint& raw) {
    const JointDef& source = rig.joints[index];
    raw.name = source.name.c_str();
    raw.transform.translation = {source.local.translation.x, source.local.translation.y, source.local.translation.z};
    raw.transform.rotation = {source.local.rotation.x, source.local.rotation.y, source.local.rotation.z, source.local.rotation.w};
    raw.transform.scale = {source.local.scale.x, source.local.scale.y, source.local.scale.z};
    for (std::size_t child : children[index]) { raw.children.emplace_back(); build_raw_children(rig, children, child, raw.children.back()); }
}

bool make_runtime_skeleton(const RigDefinition& rig, ozz::unique_ptr<ozz::animation::Skeleton>& runtime, std::vector<JointIndex>* parents, std::vector<JointRange>* subtrees, Diagnostics& diagnostics) {
    const std::vector<std::size_t> order = canonical_order(rig, diagnostics);
    if (order.empty()) return false;
    if (order.size() > kMaxJoints) { diagnostics.add("ozz-skeleton", rig.source, "rig exceeds the Matter joint limit"); return false; }
    std::unordered_map<std::string, std::size_t> authored;
    std::vector<std::vector<std::size_t>> children(rig.joints.size());
    std::size_t root = 0;
    for (std::size_t i=0; i<rig.joints.size(); ++i) authored.emplace(rig.joints[i].name, i);
    for (std::size_t i=0; i<rig.joints.size(); ++i) if (rig.joints[i].parent.empty()) root=i; else children[authored[rig.joints[i].parent]].push_back(i);
    ozz::animation::offline::RawSkeleton raw;
    raw.roots.emplace_back();
    build_raw_children(rig, children, root, raw.roots.back());
    if (!raw.Validate()) { diagnostics.add("ozz-skeleton", rig.source, "ozz rejected raw skeleton"); return false; }
    ozz::animation::offline::SkeletonBuilder builder;
    runtime = builder(raw);
    if (!runtime) { diagnostics.add("ozz-skeleton", rig.source, "ozz skeleton builder failed"); return false; }
    if (parents && subtrees) {
        parents->assign(order.size(), kInvalidJoint); subtrees->assign(order.size(), {});
        std::unordered_map<std::size_t, JointIndex> canonical;
        for (JointIndex i=0; i<order.size(); ++i) canonical[order[i]]=i;
        for (JointIndex i=0; i<order.size(); ++i) {
            const JointDef& joint=rig.joints[order[i]];
            (*parents)[i] = joint.parent.empty() ? kInvalidJoint : canonical[authored[joint.parent]];
            (*subtrees)[i].begin=i; (*subtrees)[i].end=i+1;
        }
        for (int i=static_cast<int>(order.size())-1; i>=0; --i) if ((*parents)[i] != kInvalidJoint) (*subtrees)[(*parents)[i]].end=std::max((*subtrees)[(*parents)[i]].end, (*subtrees)[i].end);
    }
    return true;
}

bool validate_metadata(const std::vector<JointIndex>& parents, const std::vector<JointRange>& subtrees) {
    const std::size_t count = parents.size();
    if (count == 0 || subtrees.size() != count || parents[0] != kInvalidJoint) return false;
    std::vector<JointRange> expected(count);
    for (std::size_t i = 0; i < count; ++i) {
        if (i != 0 && (parents[i] == kInvalidJoint || parents[i] >= i)) return false;
        if (subtrees[i].begin != i || subtrees[i].end <= i || subtrees[i].end > count) return false;
        expected[i] = {static_cast<JointIndex>(i), static_cast<JointIndex>(i + 1)};
    }
    for (std::size_t i = count; i-- > 1;) expected[parents[i]].end = std::max(expected[parents[i]].end, expected[i].end);
    return expected == subtrees;
}

bool is_ancestor(const OzzSkeleton& skeleton, JointIndex ancestor, JointIndex descendant) {
    for (JointIndex current = descendant; current != kInvalidJoint; current = skeleton.parent(current)) if (current == ancestor) return true;
    return false;
}

} // namespace

struct OzzSkeleton::Impl { ozz::unique_ptr<ozz::animation::Skeleton> runtime; std::vector<JointIndex> parents; std::vector<JointRange> subtrees; std::vector<AnimationTransform> rest_locals; };
struct OzzAnimation::Impl { ozz::unique_ptr<ozz::animation::Animation> runtime; int tracks = 0; };
struct OzzSampleContext::Impl { std::unique_ptr<ozz::animation::SamplingJob::Context> runtime; std::vector<ozz::math::SoaTransform> locals; };

OzzSkeleton::OzzSkeleton() : impl_(new Impl) {} OzzSkeleton::~OzzSkeleton() = default; OzzSkeleton::OzzSkeleton(OzzSkeleton&&) noexcept = default; OzzSkeleton& OzzSkeleton::operator=(OzzSkeleton&&) noexcept = default;
std::size_t OzzSkeleton::joint_count() const { return impl_->parents.size(); } JointIndex OzzSkeleton::parent(JointIndex joint) const { return joint < impl_->parents.size() ? impl_->parents[joint] : kInvalidJoint; } JointRange OzzSkeleton::subtree(JointIndex joint) const { return joint < impl_->subtrees.size() ? impl_->subtrees[joint] : JointRange{}; }
OzzAnimation::OzzAnimation() : impl_(new Impl) {} OzzAnimation::~OzzAnimation() = default; OzzAnimation::OzzAnimation(OzzAnimation&&) noexcept = default; OzzAnimation& OzzAnimation::operator=(OzzAnimation&&) noexcept = default;
OzzSampleContext::OzzSampleContext() : impl_(new Impl) {} OzzSampleContext::~OzzSampleContext() = default; OzzSampleContext::OzzSampleContext(OzzSampleContext&&) noexcept = default; OzzSampleContext& OzzSampleContext::operator=(OzzSampleContext&&) noexcept = default;

bool build_skeleton(const RigDefinition& rig, OzzSkeleton& skeleton, Diagnostics& diagnostics) {
    ozz::unique_ptr<ozz::animation::Skeleton> runtime; std::vector<JointIndex> parents; std::vector<JointRange> subtrees;
    if (!make_runtime_skeleton(rig, runtime, &parents, &subtrees, diagnostics)) return false;
    const std::vector<std::size_t> order = canonical_order(rig, diagnostics);
    if (order.empty()) return false;
    skeleton.impl_->rest_locals.clear();
    for (std::size_t joint : order) skeleton.impl_->rest_locals.push_back(rig.joints[joint].local);
    skeleton.impl_->runtime=std::move(runtime); skeleton.impl_->parents=std::move(parents); skeleton.impl_->subtrees=std::move(subtrees); return true;
}

bool build_clip(const RigDefinition& rig, const ClipDefinition& clip, OzzAnimation& animation, Diagnostics& diagnostics) {
    ozz::unique_ptr<ozz::animation::Skeleton> skeleton;
    if (!make_runtime_skeleton(rig, skeleton, nullptr, nullptr, diagnostics)) return false;
    const std::vector<std::size_t> order=canonical_order(rig, diagnostics); if (order.empty()) return false;
    std::unordered_map<std::string, const ClipTrack*> tracks; for (const ClipTrack& track:clip.tracks) tracks[track.joint]=&track;
    ozz::animation::offline::RawAnimation raw; raw.name=clip.name.c_str(); raw.duration=clip.duration; raw.tracks.resize(order.size());
    for (std::size_t i=0; i<order.size(); ++i) {
        const JointDef& bind=rig.joints[order[i]]; const auto found=tracks.find(bind.name); const std::vector<ClipKey>* keys=found==tracks.end()?nullptr:&found->second->keys;
        const std::size_t key_count=keys && !keys->empty()?keys->size():1;
        for (std::size_t k=0;k<key_count;++k) { const ClipKey key=keys&& !keys->empty()?(*keys)[k]:ClipKey{0.0f,bind.local,bind.source};
            raw.tracks[i].translations.push_back({key.time,{key.value.translation.x,key.value.translation.y,key.value.translation.z}});
            raw.tracks[i].rotations.push_back({key.time,{key.value.rotation.x,key.value.rotation.y,key.value.rotation.z,key.value.rotation.w}});
            raw.tracks[i].scales.push_back({key.time,{key.value.scale.x,key.value.scale.y,key.value.scale.z}}); }
    }
    if (!raw.Validate()) { diagnostics.add("ozz-animation", clip.source, "ozz rejected raw animation"); return false; }
    ozz::animation::offline::RawAnimation optimized;
    if (!ozz::animation::offline::AnimationOptimizer{}(raw, *skeleton, &optimized)) { diagnostics.add("ozz-animation", clip.source, "ozz animation optimizer failed"); return false; }
    ozz::animation::offline::AnimationBuilder builder; ozz::unique_ptr<ozz::animation::Animation> runtime=builder(optimized);
    if (!runtime) { diagnostics.add("ozz-animation", clip.source, "ozz animation builder failed"); return false; }
    animation.impl_->tracks=runtime->num_tracks(); animation.impl_->runtime=std::move(runtime); return true;
}

bool serialize_skeleton(const OzzSkeleton& skeleton, std::vector<uint8_t>& bytes) {
    if (!skeleton.impl_->runtime) { return false; }
    bytes={'M','O','S','1'}; append_u16(bytes, static_cast<uint16_t>(skeleton.impl_->parents.size()));
    for (std::size_t i=0;i<skeleton.impl_->parents.size();++i) { append_u16(bytes,skeleton.impl_->parents[i]); append_u16(bytes,skeleton.impl_->subtrees[i].begin); append_u16(bytes,skeleton.impl_->subtrees[i].end); }
    BufferStream stream; stream.Write(bytes.data(),bytes.size()); ozz::io::OArchive archive(&stream); archive << *skeleton.impl_->runtime; bytes=stream.bytes(); return true;
}
bool serialize_animation(const OzzAnimation& animation, std::vector<uint8_t>& bytes) {
    if (!animation.impl_->runtime) { return false; }
    bytes={'M','O','A','1'}; append_u16(bytes,static_cast<uint16_t>(animation.impl_->tracks)); BufferStream stream; stream.Write(bytes.data(),bytes.size()); ozz::io::OArchive archive(&stream); archive << *animation.impl_->runtime; bytes=stream.bytes(); return true;
}
bool deserialize_skeleton(const uint8_t* data, std::size_t size, OzzSkeleton& skeleton, Diagnostics& diagnostics) {
    if (!data || size<6 || std::memcmp(data,"MOS1",4)!=0) { diagnostics.add("ozz-archive",{},"invalid skeleton archive"); return false; } std::size_t offset=4; uint16_t count=0; if(!read_u16(data,size,offset,count)||count>kMaxJoints||offset+count*6>size){diagnostics.add("ozz-archive",{},"truncated skeleton archive");return false;}
    std::vector<JointIndex> parents(count); std::vector<JointRange> subtrees(count); for(uint16_t i=0;i<count;++i) if(!read_u16(data,size,offset,parents[i])||!read_u16(data,size,offset,subtrees[i].begin)||!read_u16(data,size,offset,subtrees[i].end)){diagnostics.add("ozz-archive",{},"truncated skeleton metadata");return false;}
    if (!validate_metadata(parents, subtrees)) { diagnostics.add("ozz-archive",{},"invalid skeleton metadata"); return false; }
    BufferStream stream(data,size); stream.seek(offset); ozz::unique_ptr<ozz::animation::Skeleton> runtime=ozz::make_unique<ozz::animation::Skeleton>(); ozz::io::IArchive archive(&stream); if (!archive.TestTag<ozz::animation::Skeleton>()) { diagnostics.add("ozz-archive",{},"skeleton archive tag mismatch"); return false; } archive >> *runtime; if(stream.failed() || stream.Tell() != static_cast<int>(size) || runtime->num_joints()!=count){diagnostics.add("ozz-archive",{},"skeleton metadata count mismatch");return false;} for (std::size_t i=0; i<count; ++i) if (runtime->joint_parents()[i] != (parents[i] == kInvalidJoint ? ozz::animation::Skeleton::kNoParent : static_cast<int16_t>(parents[i]))) { diagnostics.add("ozz-archive",{},"skeleton parent metadata mismatch"); return false; } std::vector<AnimationTransform> rest; std::vector<ozz::math::SoaTransform> rest_soa(runtime->joint_rest_poses().begin(), runtime->joint_rest_poses().end()); from_soa(rest_soa, count, rest); skeleton.impl_->runtime=std::move(runtime); skeleton.impl_->parents=std::move(parents); skeleton.impl_->subtrees=std::move(subtrees); skeleton.impl_->rest_locals=std::move(rest); return true;
}
bool deserialize_animation(const uint8_t* data, std::size_t size, OzzAnimation& animation, Diagnostics& diagnostics) {
    if(!data||size<6||std::memcmp(data,"MOA1",4)!=0){diagnostics.add("ozz-archive",{},"invalid animation archive");return false;} std::size_t offset=4; uint16_t tracks=0; if(!read_u16(data,size,offset,tracks)){diagnostics.add("ozz-archive",{},"truncated animation archive");return false;} BufferStream stream(data,size);stream.seek(offset);ozz::unique_ptr<ozz::animation::Animation> runtime=ozz::make_unique<ozz::animation::Animation>();ozz::io::IArchive archive(&stream);if (!archive.TestTag<ozz::animation::Animation>()) { diagnostics.add("ozz-archive",{},"animation archive tag mismatch"); return false; } archive>>*runtime;if(stream.failed() || stream.Tell() != static_cast<int>(size) || runtime->num_tracks()!=tracks){diagnostics.add("ozz-archive",{},"animation metadata count mismatch");return false;}animation.impl_->tracks=tracks;animation.impl_->runtime=std::move(runtime);return true;
}

bool sample(const OzzAnimation& animation, float ratio, OzzSampleContext& context, std::vector<AnimationTransform>& locals) {
    if(!animation.impl_->runtime || !std::isfinite(ratio)) { return false; }
    const int tracks=animation.impl_->runtime->num_tracks(); if(!context.impl_->runtime || context.impl_->runtime->max_tracks()<tracks) context.impl_->runtime.reset(new ozz::animation::SamplingJob::Context(tracks)); context.impl_->locals.resize((tracks+3)/4);
    ozz::animation::SamplingJob job; job.animation=animation.impl_->runtime.get(); job.context=context.impl_->runtime.get(); job.ratio=std::max(0.0f,std::min(1.0f,ratio)); job.output=ozz::make_span(context.impl_->locals); if(!job.Run()) return false;
    from_soa(context.impl_->locals, static_cast<std::size_t>(tracks), locals);
    return true;
}

bool blend(const OzzSkeleton& skeleton, const std::vector<BlendLayer>& layers, const std::vector<AdditiveLayer>& additive_layers, std::vector<AnimationTransform>& locals) {
    std::size_t count = 0;
    for (const BlendLayer& layer : layers) if (layer.locals) { if (count == 0) count = layer.locals->size(); if (layer.locals->size() != count) return false; }
    for (const AdditiveLayer& layer : additive_layers) if (layer.locals && (count == 0 || layer.locals->size() != count)) return false;
    if (!skeleton.impl_->runtime || count == 0 || count != skeleton.joint_count() || skeleton.impl_->rest_locals.size() != count) return false;
    std::vector<std::vector<ozz::math::SoaTransform>> poses;
    std::vector<ozz::animation::BlendingJob::Layer> normal;
    std::vector<ozz::animation::BlendingJob::Layer> additive;
    for (const BlendLayer& layer : layers) if (layer.locals) { poses.emplace_back(); to_soa(*layer.locals, poses.back()); normal.push_back({layer.weight, ozz::make_span(poses.back()), {}}); }
    for (const AdditiveLayer& layer : additive_layers) if (layer.locals) { poses.emplace_back(); to_soa(*layer.locals, poses.back()); additive.push_back({layer.weight, ozz::make_span(poses.back()), {}}); }
    std::vector<ozz::math::SoaTransform> rest;
    to_soa(skeleton.impl_->rest_locals, rest);
    std::vector<ozz::math::SoaTransform> output(rest.size());
    ozz::animation::BlendingJob job;
    job.layers = ozz::make_span(normal); job.additive_layers = ozz::make_span(additive);
    job.rest_pose = ozz::make_span(rest); job.output = ozz::make_span(output);
    if (!job.Run()) return false;
    from_soa(output, count, locals);
    return true;
}

bool local_to_model(const OzzSkeleton& skeleton, const std::vector<AnimationTransform>& locals, std::vector<Mat4f>& models, JointRange affected) {
    const std::size_t count = skeleton.joint_count();
    if (!skeleton.impl_->runtime || count == 0 || locals.size() != count) return false;
    const std::size_t begin = affected.begin == kInvalidJoint ? 0 : affected.begin;
    const std::size_t end = affected.end == kInvalidJoint ? count : affected.end;
    if (begin >= end || end > count || (begin != 0 && !(affected == skeleton.subtree(static_cast<JointIndex>(begin))))) return false;
    const bool partial = begin != 0;
    if (partial && models.size() != count) return false;
    std::vector<ozz::math::SoaTransform> input; to_soa(locals, input);
    std::vector<ozz::math::Float4x4> output(count);
    if (models.size() == count) for (std::size_t i = 0; i < count; ++i) output[i] = to_ozz_matrix(models[i]);
    ozz::animation::LocalToModelJob job;
    job.skeleton = skeleton.impl_->runtime.get(); job.input = ozz::make_span(input); job.output = ozz::make_span(output);
    job.from = static_cast<int>(begin); job.to = static_cast<int>(end - 1);
    if (!job.Run()) return false;
    if (!partial) models.resize(count);
    for (std::size_t i = begin; i < end; ++i) models[i] = from_ozz_matrix(output[i]);
    return true;
}

bool solve_two_bone(const TwoBoneSolve& solve, const std::vector<Mat4f>& models, std::vector<AnimationTransform>& locals, std::vector<Mat4f>& updated_models) {
    if(!solve.skeleton || solve.start >= solve.skeleton->joint_count() || solve.mid >= solve.skeleton->joint_count() || solve.end >= solve.skeleton->joint_count() || !is_ancestor(*solve.skeleton, solve.start, solve.mid) || !is_ancestor(*solve.skeleton, solve.mid, solve.end) || models.size()!=solve.skeleton->joint_count()||locals.size()!=models.size()) { return false; }
    const ozz::math::Float4x4 start=to_ozz_matrix(models[solve.start]),mid=to_ozz_matrix(models[solve.mid]),end=to_ozz_matrix(models[solve.end]);ozz::animation::IKTwoBoneJob job;job.target=ozz::math::simd_float4::Load3PtrU(&solve.target.x);job.pole_vector=ozz::math::simd_float4::Load3PtrU(&solve.pole_vector.x);job.mid_axis=ozz::math::simd_float4::Load3PtrU(&solve.mid_axis.x);job.twist_angle=solve.twist_angle;job.soften=solve.soften;job.weight=solve.weight;job.start_joint=&start;job.mid_joint=&mid;job.end_joint=&end;ozz::math::SimdQuaternion start_correction,mid_correction;job.start_joint_correction=&start_correction;job.mid_joint_correction=&mid_correction;if(!job.Run())return false;float values[4];ozz::math::StorePtrU(start_correction.xyzw,values);locals[solve.start].rotation=normalize(multiply(locals[solve.start].rotation,{values[0],values[1],values[2],values[3]}));ozz::math::StorePtrU(mid_correction.xyzw,values);locals[solve.mid].rotation=normalize(multiply(locals[solve.mid].rotation,{values[0],values[1],values[2],values[3]}));updated_models=models;return local_to_model(*solve.skeleton,locals,updated_models,solve.skeleton->subtree(solve.start));
}

} // namespace matter::animation
