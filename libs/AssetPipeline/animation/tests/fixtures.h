#pragma once
#include <matter/animation_asset/animation.h>
#include <cmath>
#include <stdexcept>
#include <iostream>
namespace fixture {
namespace a = matter::animation_asset;
namespace d = matter::contracts;
inline void check(bool ok, const char *message) {
  if (!ok) throw std::runtime_error(message);
}
template <class T> void good(const T &result) {
  if (!result.ok()) for (const auto &e : result.diagnostics()) std::cerr << e.message << '\n';
  check(result.ok(), "successful result");
}
inline bool near(float a, float b) { return std::fabs(a-b) < 0.0002f; }
inline matter::services::JobEnvelope envelope() { return {"animation-fixture", "revision-1", 7}; }
inline d::AssetDocument geometry() {
  d::AssetDocument doc;
  doc.id = {"fixture.animation"}; doc.provenance.id = {"source.animation"};
  doc.provenance.uri = "fixture:ANIM-EXTRACT-1";
  doc.provenance.revision = "1"; doc.provenance.content = d::sha256("animation fixture");
  doc.required_capabilities = {{"matter.asset.core", 1}, {"matter.material.pbr", 1}};
  d::MaterialDescription material; material.id = {"white"}; doc.materials.push_back(material);
  d::MeshPrototype mesh;
  mesh.id = {"triangle"}; mesh.positions = {{0,0,0},{1,0,0},{0,1,0}};
  mesh.normals.assign(3, {0,0,1}); mesh.indices = {0,1,2};
  mesh.submeshes = {{0,3,material.id}}; mesh.bounds = {{0,0,0},{1,1,0}};
  doc.meshes = {mesh}; doc.bounds = mesh.bounds;
  return doc;
}
inline d::AssetDocument animated() {
  auto doc = geometry(); doc.required_capabilities.push_back({"matter.asset.animation",1});
  doc.rigs = {{{"rig"}, {{{"root"},-1,{}}, {{"tip"},0,mm::translation({-2,0,0})}}}};
  d::SkinWeights skin; skin.prototype = {"triangle"}; skin.rig = {"rig"};
  skin.joints.assign(3, {0,1,0,0}); skin.weights.assign(3, {0.25f,0.75f,0,0});
  doc.skins = {skin};
  d::AnimationClip clip; clip.id = {"move"}; clip.rig = {"rig"}; clip.duration_seconds = 1;
  d::AnimationTrack track; track.joint = {"tip"};
  d::AnimationKey start, end; start.translation = {2,0,0}; end = start;
  end.time_seconds = 1; end.translation.x = 3; track.keys = {start,end};
  clip.tracks = {track}; doc.animations = {clip};
  return doc;
}
}
