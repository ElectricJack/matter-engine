#include "fixtures.h"
int main() try {
  using namespace fixture;
  auto doc = animated();
  a::Bindings bindings;
  bindings.rigid = {{{"piece"},{"triangle"},{"rig"},{"tip"},mm::translation({0,3,0})}};
  auto result = a::prepare(envelope(), doc, bindings); good(result);
  check(result.value()->document.animations[0].tracks[0].keys[1].translation.x == 3,
        "ANIM-PORTABLE: clip keys retained");
  doc.skins[0].weights[0].x = 1;
  check(result.value()->document.skins[0].weights[0].x == 0.25f, "owned immutable data");
  a::PoseFrame pose{envelope(), {"actor"}, 9, mm::translation({10,0,0}), {},
      {{{"rig"}, {{},mm::translation({4,0,0})}, {{},mm::translation({2,0,0})}}}};
  auto frame = a::graphics_frame(result, pose, 9); good(frame);
  check(near(frame.value()->rigid[0].world.m[3],14) && near(frame.value()->rigid[0].world.m[7],3),
        "ANIM-PIVOT: entity * joint * pivot");
  check(near(frame.value()->rigid[0].previous_world.m[3],2), "previous placement retained");
  check(near(frame.value()->skins[0].current[1].position.m[3],2), "model * inverse bind palette");
  check(near(frame.value()->skins[0].previous[1].position.m[3],0), "bind pose identity palette");
  pose.rigs[0].model[1] = mm::from_trs({4,0,0}, {0,0,0.70710678f,0.70710678f}, {1,1,1});
  frame = a::graphics_frame(result, pose, 9); good(frame);
  check(near(frame.value()->rigid[0].world.m[3],11) && near(frame.value()->rigid[0].world.m[7],0),
        "rotated joint applies pivot in joint-local space");
  pose.rigs[0].model[0] = mm::from_trs({}, {}, {-2,3,4});
  frame = a::graphics_frame(result, pose, 9); good(frame);
  check(near(frame.value()->skins[0].current[0].normal.m[0],-0.5f) &&
        near(frame.value()->skins[0].current[0].normal.m[5],1.0f/3), "mirrored nonuniform normal palette");
  check(!a::graphics_frame(result, pose, 10).ok(), "stale serial rejected");
  ++pose.asset.generation_epoch;
  check(!a::graphics_frame(result, pose, 9).ok(), "stale revision envelope rejected");
  pose.asset = envelope(); pose.rigs[0].model.pop_back();
  check(!a::graphics_frame(result, pose, 9).ok(), "partial pose rejected");
  check(a::supports(*result.value(), a::Consumer::browser_pose).ok(), "portable browser pose support");
  check(!a::supports(*result.value(), a::Consumer::gltf_export).ok(), "animation export explicitly unsupported");
  bindings.requires_native_motion = true;
  auto native = a::prepare(envelope(), animated(), bindings); good(native);
  check(!a::supports(*native.value(), a::Consumer::browser_pose).ok(), "native motion browser rejection");
  auto bad = animated(); bad.rigs[0].joints[0].parent = 1;
  check(!a::prepare(envelope(),bad).ok(), "rig cycle rejected");
  bad = animated(); bad.skins[0].joints[0][0] = 256;
  check(!a::prepare(envelope(),bad).ok(), "bad weight joint rejected");
  bad = animated(); bad.skins[0].weights[0].x = -1;
  check(!a::prepare(envelope(),bad).ok(), "bad weights rejected");
  bad = animated(); bad.required_capabilities.push_back({"unknown.animation",1});
  check(!a::prepare(envelope(),bad).ok(), "unknown required capability rejected");
  bad = animated(); bad.animations[0].tracks[0].keys[1].time_seconds = 0;
  check(!a::prepare(envelope(),bad).ok(), "unordered clip times rejected");
  a::Options limits; limits.max_joints_per_rig = 1;
  check(!a::prepare(envelope(),animated(),{},limits).ok(), "joint budget rejected");
  limits = {}; limits.max_skinned_vertices = 2;
  check(!a::prepare(envelope(),animated(),{},limits).ok(), "vertex budget rejected");
  matter::services::CancellationSource cancellation; cancellation.cancel();
  matter::services::JobControl control; control.cancellation = cancellation.token();
  check(!a::prepare(envelope(),animated(),{}, {},control).ok(), "cancel publishes no result");
  auto forged_value = *result.value(); forged_value.bindings.rigid[0].rig = {"missing"};
  auto forged = a::PreparedResult::success(envelope(),std::move(forged_value));
  pose.rigs[0].model = {{},mm::translation({2,0,0})};
  check(!a::graphics_frame(forged,pose,9).ok(), "invalid externally constructed placement fails safely");
  std::cout << "ANIM-PORTABLE ANIM-PIVOT ANIM-BOUNDARY PASS\n";
} catch (const std::exception &error) {
  std::cerr << "FAIL: " << error.what() << '\n'; return 1;
}
