#include "dsl_state.h"
#include "dsl_bindings.h"
#include "pf_bindings.h"
#include "tileset_spec.h"
#include "tileset_placement.h"
#include "tileset_layout.h"
#include "part_graph.h"   // params_from_json, params_to_json — canonical JSON normalizer
#include "terrain_mesher.h"
#include "triangle_emit.hpp"
#include <cmath>
#include <cstring>
#include <vector>
#include <regex>
extern "C" {
#include "quickjs.h"
}

// Per-part SCHEMA-level configuration (lodBudgets, lodAnchorSize) is
// discovered via `static` class properties read by ScriptHost — NOT via the
// runtime verb bindings below. If you're looking for the lodBudgets binding,
// see: MatterEngine3/src/script_host.cpp :: eval_lod_budgets
// This file only owns the runtime-verb bindings the Part class methods forward
// to (translate, box, sphere, placeChild, tileset verbs, etc.).

namespace dsl {

// Normalize raw JS_JSONStringify bytes to the canonical params_to_json format
// (sorted keys, %.17g numbers, no whitespace) so that composite child-hash
// lookups (`module \x1f params_json`) match the keys the host writes into
// name2hash (part_graph.cpp: params_to_json(kid.params)).
// An empty input returns an empty string so callers can distinguish "no params"
// from "params that happen to parse to an empty map".
static std::string normalize_params_json(const char* s, size_t len) {
    if (!s || len == 0) return {};
    part_graph::Params p = part_graph::params_from_json(std::string(s, len));
    return part_graph::params_to_json(p);
}

static DslState* state_of(JSContext* ctx) {
    return static_cast<DslState*>(JS_GetContextOpaque(ctx));
}
static double argd(JSContext* ctx, JSValueConst v) { double d=0; JS_ToFloat64(ctx,&d,v); return d; }

static JSValue j_pushMatrix(JSContext* c, JSValueConst, int, JSValueConst*) { state_of(c)->pushMatrix(); return JS_UNDEFINED; }
static JSValue j_popMatrix (JSContext* c, JSValueConst, int, JSValueConst*) { state_of(c)->popMatrix();  return JS_UNDEFINED; }
static JSValue j_translate(JSContext* c, JSValueConst, int n, JSValueConst* a){ state_of(c)->translate(argd(c,a[0]),argd(c,a[1]),argd(c,a[2])); return JS_UNDEFINED; }
static JSValue j_rotateX(JSContext* c, JSValueConst, int, JSValueConst* a){ state_of(c)->rotateX(argd(c,a[0])); return JS_UNDEFINED; }
static JSValue j_rotateY(JSContext* c, JSValueConst, int, JSValueConst* a){ state_of(c)->rotateY(argd(c,a[0])); return JS_UNDEFINED; }
static JSValue j_rotateZ(JSContext* c, JSValueConst, int, JSValueConst* a){ state_of(c)->rotateZ(argd(c,a[0])); return JS_UNDEFINED; }
static JSValue j_scale(JSContext* c, JSValueConst, int, JSValueConst* a){ state_of(c)->scale(argd(c,a[0]),argd(c,a[1]),argd(c,a[2])); return JS_UNDEFINED; }
static JSValue j_applyMatrix(JSContext* c, JSValueConst, int, JSValueConst* a){
    float m[16]; for (int i=0;i<16;++i){ JSValue e=JS_GetPropertyUint32(c,a[0],i); m[i]=(float)argd(c,e); JS_FreeValue(c,e);} state_of(c)->applyMatrix(m); return JS_UNDEFINED; }
static JSValue j_fill(JSContext* c, JSValueConst, int, JSValueConst* a){ int32_t id=0; JS_ToInt32(c,&id,a[0]); state_of(c)->fill((uint32_t)id); return JS_UNDEFINED; }
static JSValue j_tint(JSContext* c, JSValueConst, int, JSValueConst* a){
    state_of(c)->tint((float)argd(c,a[0]),(float)argd(c,a[1]),(float)argd(c,a[2]),(float)argd(c,a[3])); return JS_UNDEFINED; }
static JSValue j_lookAt(JSContext* c, JSValueConst, int n, JSValueConst* a){
    // lookAt(tx,ty,tz, [upx,upy,upz]); up defaults to +Y when omitted.
    float ux = (n>3)?(float)argd(c,a[3]):0.0f;
    float uy = (n>3)?(float)argd(c,a[4]):1.0f;
    float uz = (n>3)?(float)argd(c,a[5]):0.0f;
    state_of(c)->lookAt((float)argd(c,a[0]),(float)argd(c,a[1]),(float)argd(c,a[2]),ux,uy,uz);
    return JS_UNDEFINED; }
static JSValue j_beginVoxels(JSContext* c, JSValueConst, int, JSValueConst* a){ state_of(c)->beginVoxels((float)argd(c,a[0])); return JS_UNDEFINED; }
static JSValue j_endVoxels(JSContext* c, JSValueConst, int, JSValueConst*){ state_of(c)->endVoxels(); return JS_UNDEFINED; }

static std::string arg_string(JSContext* c, JSValueConst value) {
    const char* text = JS_ToCString(c, value); if (!text) return {};
    std::string result(text); JS_FreeCString(c, text); return result;
}
// Every Part rig wrapper deliberately appends Error.stack after its authored
// arguments. Keep that hidden diagnostic argument out of native arity checks;
// an authored `undefined` remains an actual supplied user argument.
static int rig_user_argc(int argc) { return argc > 0 ? argc - 1 : 0; }
static bool rig_nonempty_string(JSContext* c, int argc, JSValueConst* args, int index, std::string& out) {
    if (index >= argc || !JS_IsString(args[index])) return false;
    out = arg_string(c, args[index]);
    return !out.empty();
}
static bool rig_optional_string(JSContext* c, int argc, JSValueConst* args, int index, std::string& out) {
    if (index >= argc || JS_IsUndefined(args[index]) || JS_IsNull(args[index])) { out.clear(); return true; }
    if (!JS_IsString(args[index])) return false;
    out = arg_string(c, args[index]);
    return true;
}
static bool rig_finite_number(JSContext* c, int argc, JSValueConst* args, int index, float& out) {
    if (index >= argc || !JS_IsNumber(args[index])) return false;
    double value = 0.0;
    if (JS_ToFloat64(c, &value, args[index]) < 0 || !std::isfinite(value)) return false;
    out = static_cast<float>(value);
    return true;
}
static bool array_floats(JSContext* c, JSValueConst value, int count, float* out) {
    if (!JS_IsArray(value)) return false;
    for (int i=0; i<count; ++i) { JSValue item=JS_GetPropertyUint32(c,value,i); double number=0; const bool numeric=JS_IsNumber(item); const int ok=numeric ? JS_ToFloat64(c,&number,item) : -1; JS_FreeValue(c,item); if (ok < 0 || !std::isfinite(number)) return false; out[i]=static_cast<float>(number); }
    return true;
}
static matter::AnimationTransform rig_transform(JSContext* c, JSValueConst position, JSValueConst rotation, bool required_position, bool* ok) {
    matter::AnimationTransform result{}; *ok = !required_position && (JS_IsUndefined(position) || JS_IsNull(position));
    if (!*ok) *ok = array_floats(c,position,3,&result.translation.x);
    if (*ok && !JS_IsUndefined(rotation) && !JS_IsNull(rotation)) *ok = array_floats(c,rotation,4,&result.rotation.x);
    return result;
}
static matter::AnimationTransform socket_transform(JSContext* c, JSValueConst value, bool* ok) {
    matter::AnimationTransform result{}; *ok=true;
    if (JS_IsUndefined(value) || JS_IsNull(value)) return result;
    if (!JS_IsObject(value)) { *ok=false; return result; }
    JSValue p=JS_GetPropertyStr(c,value,"position"); if (JS_IsUndefined(p)) { JS_FreeValue(c,p); p=JS_GetPropertyStr(c,value,"translation"); }
    JSValue r=JS_GetPropertyStr(c,value,"rotation"), s=JS_GetPropertyStr(c,value,"scale");
    if (!JS_IsUndefined(p) && !JS_IsNull(p)) *ok=array_floats(c,p,3,&result.translation.x);
    if (*ok && !JS_IsUndefined(r) && !JS_IsNull(r)) *ok=array_floats(c,r,4,&result.rotation.x);
    if (*ok && !JS_IsUndefined(s) && !JS_IsNull(s)) *ok=array_floats(c,s,3,&result.scale.x);
    JS_FreeValue(c,p); JS_FreeValue(c,r); JS_FreeValue(c,s); return result;
}
static void rig_source(JSContext* c, int n, JSValueConst* a, const char* object) {
    matter::animation::SourceSpan source{"<part>", 0, 0, object};
    if (n > 0 && JS_IsString(a[n-1])) {
        const std::string stack=arg_string(c,a[n-1]);
        // The Part base wrapper creates Error.stack itself, so its frame is
        // always first.  Select the first authored caller frame instead of
        // searching only for the root pseudo-module: imported shared-lib
        // declarations must retain their canonical module specifier.
        const std::regex frame("([^()\\s:]+):(\\d+):(\\d+)");
        for (std::sregex_iterator it(stack.begin(), stack.end(), frame), end; it != end; ++it) {
            const std::string module=(*it)[1];
            if (module == "<part-base>") continue;
            source.module=module;
            source.line=static_cast<uint32_t>(std::stoul((*it)[2]));
            source.column=static_cast<uint32_t>(std::stoul((*it)[3]));
            break;
        }
    }
    state_of(c)->set_rig_source(std::move(source));
}
static int anim_user_argc(int argc) { return argc > 0 ? argc - 1 : 0; }
static bool anim_string(JSContext* c, JSValueConst v, std::string& out) { if(!JS_IsString(v))return false; out=arg_string(c,v); return !out.empty(); }
static bool anim_cadence(JSContext* c, JSValueConst value, matter::animation::EvaluationCadence& out) {
    if (!JS_IsString(value)) return false;
    const std::string name = arg_string(c, value);
    if (name == "fixed") { out = matter::animation::EvaluationCadence::Fixed; return true; }
    if (name == "frame") { out = matter::animation::EvaluationCadence::Frame; return true; }
    return false;
}
static bool anim_value(JSContext* c, JSValueConst value, matter::AnimationValueType type,
                       matter::animation::AnimationValue& out) {
    out = {}; out.type = type;
    if (type == matter::AnimationValueType::Bool) {
        if (!JS_IsBool(value)) return false;
        out.boolean = JS_ToBool(c, value) > 0;
    } else if (type == matter::AnimationValueType::Number) {
        double number = 0.0;
        if (!JS_IsNumber(value) || JS_ToFloat64(c, &number, value) < 0 || !std::isfinite(number)) return false;
        out.number = number;
    } else if (type == matter::AnimationValueType::Symbol) {
        if (!JS_IsString(value)) return false;
        out.symbol = arg_string(c, value);
    } else if (type == matter::AnimationValueType::Float3) {
        if (!array_floats(c, value, 3, &out.float3.x)) return false;
    } else if (type == matter::AnimationValueType::Quaternion) {
        if (!array_floats(c, value, 4, &out.quaternion.x)) return false;
    } else if (type == matter::AnimationValueType::Transform) {
        bool ok = false; out.transform = socket_transform(c, value, &ok); if (!ok) return false;
    }
    return true;
}
static bool anim_type(JSContext* c, JSValueConst value, matter::AnimationValueType& out) {
    if (!JS_IsString(value)) return false;
    const std::string name = arg_string(c, value);
    if (name == "float" || name == "number") { out = matter::AnimationValueType::Number; return true; }
    if (name == "bool" || name == "boolean") { out = matter::AnimationValueType::Bool; return true; }
    if (name == "vec3" || name == "float3") { out = matter::AnimationValueType::Float3; return true; }
    if (name == "quat" || name == "quaternion") { out = matter::AnimationValueType::Quaternion; return true; }
    if (name == "transform") { out = matter::AnimationValueType::Transform; return true; }
    if (name == "symbol" || name == "enum") { out = matter::AnimationValueType::Symbol; return true; }
    return false;
}
static JSValue j_beginClip(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    rig_source(c,n,a,"beginClip"); int argc=anim_user_argc(n); int off=(argc>1&&JS_IsNumber(a[0]))?1:0; std::string name; if(argc<=off||!anim_string(c,a[off],name)){state_of(c)->set_rig_error("beginClip requires a name");return JS_UNDEFINED;} float duration=1,rate=30; bool loop=false,add=false; if(argc>off+1&&JS_IsObject(a[off+1])){JSValue v=JS_GetPropertyStr(c,a[off+1],"duration");if(JS_IsNumber(v))duration=(float)argd(c,v);JS_FreeValue(c,v);v=JS_GetPropertyStr(c,a[off+1],"sampleRate");if(JS_IsNumber(v))rate=(float)argd(c,v);JS_FreeValue(c,v);v=JS_GetPropertyStr(c,a[off+1],"loop");if(!JS_IsUndefined(v))loop=JS_ToBool(c,v)>0;JS_FreeValue(c,v);v=JS_GetPropertyStr(c,a[off+1],"mode");if(JS_IsString(v))add=arg_string(c,v)=="additive";JS_FreeValue(c,v);} state_of(c)->begin_clip(name,duration,rate,loop,add); return JS_UNDEFINED;
}
static JSValue j_clipDuration(JSContext* c, JSValueConst, int n, JSValueConst* a){int argc=n;float v=0;if(argc<1||!rig_finite_number(c,argc,a,0,v))state_of(c)->set_rig_error("duration requires a finite number");else state_of(c)->clip_duration(v);return JS_UNDEFINED;}
static JSValue j_clipRate(JSContext* c, JSValueConst, int n, JSValueConst* a){int argc=n;float v=0;if(argc<1||!rig_finite_number(c,argc,a,0,v))state_of(c)->set_rig_error("sampleRate requires a finite number");else state_of(c)->clip_rate(v);return JS_UNDEFINED;}
static JSValue j_clipLoop(JSContext* c, JSValueConst, int n, JSValueConst* a){int argc=n;state_of(c)->clip_loop(argc>0&&JS_ToBool(c,a[0])>0);return JS_UNDEFINED;}
static JSValue j_clipMode(JSContext* c, JSValueConst, int n, JSValueConst* a){int argc=n;state_of(c)->clip_mode(argc>0&&JS_IsString(a[0])&&arg_string(c,a[0])=="additive");return JS_UNDEFINED;}
static JSValue j_clipAt(JSContext* c, JSValueConst, int n, JSValueConst* a){int argc=n;std::string s;if(argc<1||!anim_string(c,a[0],s))state_of(c)->set_rig_error("clip at requires a joint name");else state_of(c)->clip_at(s);return JS_UNDEFINED;}
static JSValue j_clipMarker(JSContext* c, JSValueConst, int n, JSValueConst* a){rig_source(c,n,a,"marker");int argc=anim_user_argc(n);std::string s;float t=0;if(argc<2||!rig_finite_number(c,argc,a,0,t)||!anim_string(c,a[1],s))state_of(c)->set_rig_error("marker requires finite time and name");else state_of(c)->clip_marker(t,s);return JS_UNDEFINED;}
static JSValue j_clipKey(JSContext* c, JSValueConst, int n, JSValueConst* a){rig_source(c,n,a,"key");int argc=anim_user_argc(n);std::string s;float t=0;bool ok=argc>=3&&anim_string(c,a[0],s)&&rig_finite_number(c,argc,a,1,t);bool tr=false;matter::AnimationTransform v=socket_transform(c,a[2],&tr);if(!ok||!tr)state_of(c)->set_rig_error("key requires joint, finite time, and transform");else state_of(c)->clip_key(s,t,v);return JS_UNDEFINED;}
static JSValue j_generate(JSContext* c, JSValueConst, int n, JSValueConst* a){int argc=n;if(argc<1||!JS_IsFunction(c,a[0])){state_of(c)->set_rig_error("generate requires a callback");return JS_UNDEFINED;} DslState* st=state_of(c); const uint32_t seg=st->clip_sample_segments(); if(seg==0){st->set_rig_error("generated clip requires finite positive duration and sampleRate");return JS_UNDEFINED;} const uint32_t count=st->clip_is_loop()?seg:seg+1; for(uint32_t i=0;i<count;++i){ if(!st->begin_clip_sample())break; const float phase=(float)i/(float)seg; JSValue pv=JS_NewFloat64(c,phase); JSValue r=JS_Call(c,a[0],JS_UNDEFINED,1,&pv); JS_FreeValue(c,pv); if(JS_IsException(r)){JS_FreeValue(c,r);st->set_rig_error("generated clip callback failed");break;} JS_FreeValue(c,r); if(!st->capture_clip_sample(phase))break; } return JS_UNDEFINED; }
static JSValue j_endClip(JSContext* c, JSValueConst, int n, JSValueConst* a){rig_source(c,n,a,"endClip");state_of(c)->end_clip();return JS_UNDEFINED;}
static JSValue j_beginMotion(JSContext* c, JSValueConst, int n, JSValueConst* a){rig_source(c,n,a,"beginMotion");int argc=anim_user_argc(n);std::string s="motion";if(argc>0&&!JS_IsUndefined(a[0])&&!anim_string(c,a[0],s))state_of(c)->set_rig_error("beginMotion requires a name");else state_of(c)->begin_motion(s);return JS_UNDEFINED;}
static JSValue j_motionInput(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    rig_source(c,n,a,"input"); const int argc=anim_user_argc(n); std::string name;
    if(argc<2||!anim_string(c,a[0],name)||!JS_IsObject(a[1])) { state_of(c)->set_rig_error("input requires name and options"); return JS_UNDEFINED; }
    matter::animation::InputSchema in; in.name=name; in.source=state_of(c)->rig_source(); in.source.object="input";
    JSValue value=JS_GetPropertyStr(c,a[1],"type"); const bool type_ok=anim_type(c,value,in.type); JS_FreeValue(c,value);
    if(!type_ok) { state_of(c)->set_rig_error("input type must be a supported type string"); return JS_UNDEFINED; }
    value=JS_GetPropertyStr(c,a[1],"cadence");
    if(!JS_IsUndefined(value) && !anim_cadence(c,value,in.cadence)) { JS_FreeValue(c,value); state_of(c)->set_rig_error("input cadence must be 'fixed' or 'frame'"); return JS_UNDEFINED; }
    JS_FreeValue(c,value); value=JS_GetPropertyStr(c,a[1],"default");
    if(!JS_IsUndefined(value) && !anim_value(c,value,in.type,in.default_value)) { JS_FreeValue(c,value); state_of(c)->set_rig_error("input default does not match its declared type"); return JS_UNDEFINED; }
    JS_FreeValue(c,value); state_of(c)->motion_input(in); return JS_UNDEFINED;
}
static JSValue j_motionTarget(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    rig_source(c,n,a,"target"); const int argc=anim_user_argc(n); std::string name;
    if(argc<2||!anim_string(c,a[0],name)||!JS_IsObject(a[1])) { state_of(c)->set_rig_error("target requires name and options"); return JS_UNDEFINED; }
    matter::animation::TargetSchema t; t.name=name; t.source=state_of(c)->rig_source(); t.source.object="target";
    auto string_property=[&](const char* key,std::string& out){ JSValue value=JS_GetPropertyStr(c,a[1],key); const bool ok=anim_string(c,value,out); JS_FreeValue(c,value); return ok; };
    if(!string_property("start",t.start_joint)||!string_property("end",t.end_joint)) { state_of(c)->set_rig_error("target requires start and end joints"); return JS_UNDEFINED; }
    JSValue value=JS_GetPropertyStr(c,a[1],"cadence");
    if(!JS_IsUndefined(value) && !anim_cadence(c,value,t.cadence)) { JS_FreeValue(c,value); state_of(c)->set_rig_error("target cadence must be 'fixed' or 'frame'"); return JS_UNDEFINED; }
    JS_FreeValue(c,value); value=JS_GetPropertyStr(c,a[1],"driver");
    if(!JS_IsUndefined(value)) {
        if(JS_IsString(value)) { const std::string driver=arg_string(c,value); if(driver=="external") t.driver=matter::animation::TargetDriverKind::External; else if(driver=="controller") t.driver=matter::animation::TargetDriverKind::Controller; else { JS_FreeValue(c,value); state_of(c)->set_rig_error("target driver must be 'external' or 'controller'"); return JS_UNDEFINED; } }
        else if(JS_IsObject(value)) { JSValue controller=JS_GetPropertyStr(c,value,"controller"); const bool ok=anim_string(c,controller,t.controller); JS_FreeValue(c,controller); if(!ok) { JS_FreeValue(c,value); state_of(c)->set_rig_error("target controller driver requires a controller name"); return JS_UNDEFINED; } t.driver=matter::animation::TargetDriverKind::Controller; }
        else { JS_FreeValue(c,value); state_of(c)->set_rig_error("target driver must be a string or controller object"); return JS_UNDEFINED; }
    }
    JS_FreeValue(c,value); value=JS_GetPropertyStr(c,a[1],"pole");
    if(!JS_IsUndefined(value)&&!JS_IsNull(value)) { if(!array_floats(c,value,3,&t.pole.x)) { JS_FreeValue(c,value); state_of(c)->set_rig_error("target pole must be a finite vec3"); return JS_UNDEFINED; } t.has_pole=true; }
    JS_FreeValue(c,value); auto number_property=[&](const char* key,float& out){ JSValue number=JS_GetPropertyStr(c,a[1],key); if(JS_IsUndefined(number)) { JS_FreeValue(c,number); return true; } double parsed=0.0; const bool ok=JS_IsNumber(number)&&JS_ToFloat64(c,&parsed,number)>=0&&std::isfinite(parsed); JS_FreeValue(c,number); if(ok) out=static_cast<float>(parsed); return ok; };
    if(!number_property("soften",t.soften)||!number_property("twist",t.twist)||!number_property("positionHalfLife",t.position_half_life)||!number_property("rotationHalfLife",t.rotation_half_life)||!number_property("weightHalfLife",t.weight_half_life)) { state_of(c)->set_rig_error("target numeric options must be finite numbers"); return JS_UNDEFINED; }
    value=JS_GetPropertyStr(c,a[1],"enabled"); if(!JS_IsUndefined(value)) { if(!JS_IsBool(value)) { JS_FreeValue(c,value); state_of(c)->set_rig_error("target enabled must be boolean"); return JS_UNDEFINED; } t.enabled=JS_ToBool(c,value)>0; } JS_FreeValue(c,value);
    state_of(c)->motion_target(t); return JS_UNDEFINED;
}
static JSValue j_motionController(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    rig_source(c,n,a,"controller"); const int argc=anim_user_argc(n); std::string name,type="native";
    if(argc<1||!anim_string(c,a[0],name)) { state_of(c)->set_rig_error("controller requires a name"); return JS_UNDEFINED; }
    if(argc>1 && !anim_string(c,a[1],type)) { state_of(c)->set_rig_error("controller type must be a non-empty string"); return JS_UNDEFINED; }
    matter::animation::ControllerDef d; d.name=name; d.type=type; d.source=state_of(c)->rig_source(); d.source.object="controller";
    if(argc>2) { if(!JS_IsObject(a[2])) { state_of(c)->set_rig_error("controller options must be an object"); return JS_UNDEFINED; } JSValue value=JS_GetPropertyStr(c,a[2],"cadence"); if(!JS_IsUndefined(value)&&!anim_cadence(c,value,d.cadence)) { JS_FreeValue(c,value); state_of(c)->set_rig_error("controller cadence must be 'fixed' or 'frame'"); return JS_UNDEFINED; } JS_FreeValue(c,value); }
    state_of(c)->motion_controller(d); return JS_UNDEFINED;
}
static JSValue j_motionNode(JSContext* c, JSValueConst, int n, JSValueConst* a, matter::animation::GraphNodeKind kind){rig_source(c,n,a,"graph");int argc=anim_user_argc(n);std::string name;if(argc<1||!anim_string(c,a[0],name)){state_of(c)->set_rig_error("graph node requires a name");return JS_UNDEFINED;}matter::animation::GraphNode g;g.name=name;g.kind=kind;g.source=state_of(c)->rig_source();g.source.object="graph";for(int i=1;i<argc;++i){std::string dep;if(anim_string(c,a[i],dep))g.dependencies.push_back(dep);}g.is_output=kind==matter::animation::GraphNodeKind::Output;state_of(c)->motion_node(g);return JS_UNDEFINED;}
static JSValue j_clipNode(JSContext* c,JSValueConst,int n,JSValueConst*a){rig_source(c,n,a,"clipNode");int argc=anim_user_argc(n);if(argc<2){state_of(c)->set_rig_error("clipNode requires name and clip");return JS_UNDEFINED;}matter::animation::GraphNode g;g.name=arg_string(c,a[0]);g.clip=arg_string(c,a[1]);g.kind=matter::animation::GraphNodeKind::Clip;g.source=state_of(c)->rig_source();g.source.object="clipNode";state_of(c)->motion_node(g);return JS_UNDEFINED;} static JSValue j_blendNode(JSContext*c,JSValueConst,int n,JSValueConst*a){rig_source(c,n,a,"blend1D");int argc=anim_user_argc(n);if(argc<3||!JS_IsArray(a[2])){state_of(c)->set_rig_error("blend1D requires name, input, and samples");return JS_UNDEFINED;}matter::animation::GraphNode g;g.name=arg_string(c,a[0]);g.input=arg_string(c,a[1]);g.kind=matter::animation::GraphNodeKind::Blend1D;g.source=state_of(c)->rig_source();g.source.object="blend1D";JSValue lv=JS_GetPropertyStr(c,a[2],"length");uint32_t len=0;JS_ToUint32(c,&len,lv);JS_FreeValue(c,lv);for(uint32_t i=0;i<len;++i){JSValue pair=JS_GetPropertyUint32(c,a[2],i);if(JS_IsArray(pair)){JSValue tv=JS_GetPropertyUint32(c,pair,0);if(JS_IsNumber(tv))g.thresholds.push_back((float)argd(c,tv));JS_FreeValue(c,tv);JSValue nv=JS_GetPropertyUint32(c,pair,1);std::string dep;if(anim_string(c,nv,dep))g.dependencies.push_back(dep);JS_FreeValue(c,nv);}JS_FreeValue(c,pair);}state_of(c)->motion_node(g);return JS_UNDEFINED;} static JSValue j_additiveNode(JSContext*c,JSValueConst,int n,JSValueConst*a){return j_motionNode(c,JS_UNDEFINED,n,a,matter::animation::GraphNodeKind::Additive);} static JSValue j_nativeNode(JSContext*c,JSValueConst,int n,JSValueConst*a){rig_source(c,n,a,"nativeController");int argc=anim_user_argc(n);if(argc<2){state_of(c)->set_rig_error("nativeController requires name and controller");return JS_UNDEFINED;}matter::animation::GraphNode g;g.name=arg_string(c,a[0]);g.controller=arg_string(c,a[1]);g.kind=matter::animation::GraphNodeKind::NativeController;g.source=state_of(c)->rig_source();g.source.object="nativeController";if(argc>2){std::string dep;if(anim_string(c,a[2],dep))g.dependencies.push_back(dep);}state_of(c)->motion_node(g);return JS_UNDEFINED;} static JSValue j_outputNode(JSContext*c,JSValueConst,int n,JSValueConst*a){return j_motionNode(c,JS_UNDEFINED,n,a,matter::animation::GraphNodeKind::Output);}
static JSValue j_endMotion(JSContext*c,JSValueConst,int n,JSValueConst*a){rig_source(c,n,a,"endMotion");state_of(c)->end_motion();return JS_UNDEFINED;}
static JSValue j_beginRig(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    rig_source(c,n,a,"beginRig"); const int argc=rig_user_argc(n); std::string name;
    if (!rig_optional_string(c,argc,a,0,name)) { state_of(c)->set_rig_error("beginRig name must be a string"); return JS_NewInt64(c,0); }
    return JS_NewInt64(c,(int64_t)state_of(c)->begin_rig(name));
}
static JSValue j_root(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    rig_source(c,n,a,"root"); const int argc=rig_user_argc(n); std::string name;
    if (!rig_nonempty_string(c,argc,a,0,name)) state_of(c)->set_rig_error("root requires a non-empty string name");
    else { bool ok=false; const auto local=rig_transform(c,argc>1?a[1]:JS_UNDEFINED,argc>2?a[2]:JS_UNDEFINED,false,&ok); if (!ok) state_of(c)->set_rig_error("root requires finite position and rotation arrays"); else state_of(c)->rig_root(name,local); }
    return JS_UNDEFINED;
}
static JSValue j_bone(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    rig_source(c,n,a,"bone"); const int argc=rig_user_argc(n); std::string name;
    if (!rig_nonempty_string(c,argc,a,0,name)) state_of(c)->set_rig_error("bone requires a non-empty string name");
    else if (argc <= 1 || !JS_IsArray(a[1])) state_of(c)->set_rig_error("bone requires finite endpoint and rotation arrays");
    else { bool ok=false; const auto local=rig_transform(c,a[1],argc>2?a[2]:JS_UNDEFINED,true,&ok); if (!ok) state_of(c)->set_rig_error("bone requires finite endpoint and rotation arrays"); else state_of(c)->rig_bone(name,local); }
    return JS_UNDEFINED;
}
static JSValue j_rigPush(JSContext* c, JSValueConst, int n, JSValueConst* a) { rig_source(c,n,a,"push"); state_of(c)->rig_push(); return JS_UNDEFINED; }
static JSValue j_rigPop(JSContext* c, JSValueConst, int n, JSValueConst* a) { rig_source(c,n,a,"pop"); state_of(c)->rig_pop(); return JS_UNDEFINED; }
static JSValue j_atJoint(JSContext* c, JSValueConst, int n, JSValueConst* a) { rig_source(c,n,a,"atJoint"); const int argc=rig_user_argc(n); std::string name; if (!rig_nonempty_string(c,argc,a,0,name)) state_of(c)->set_rig_error("atJoint requires a non-empty string name"); else state_of(c)->rig_at_joint(name); return JS_UNDEFINED; }
static JSValue j_radius(JSContext* c, JSValueConst, int n, JSValueConst* a) { rig_source(c,n,a,"radius"); const int argc=rig_user_argc(n); float value=0.0f; if (!rig_finite_number(c,argc,a,0,value)) state_of(c)->set_rig_error("radius requires a finite numeric value"); else state_of(c)->rig_radius(value); return JS_UNDEFINED; }
static JSValue j_socket(JSContext* c, JSValueConst, int n, JSValueConst* a) { rig_source(c,n,a,"socket"); const int argc=rig_user_argc(n); std::string name; if (!rig_nonempty_string(c,argc,a,0,name)) state_of(c)->set_rig_error("socket requires a non-empty string name"); else { bool ok=false; const auto local=socket_transform(c,argc>1?a[1]:JS_UNDEFINED,&ok); if (!ok) state_of(c)->set_rig_error("socket requires a finite transform object"); else state_of(c)->rig_socket(name,local); } return JS_UNDEFINED; }
static JSValue j_mirrorBranch(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    rig_source(c,n,a,"mirrorBranch"); DslState* state=state_of(c); const int argc=rig_user_argc(n); std::string from,to;
    if (!rig_nonempty_string(c,argc,a,0,from) || !rig_nonempty_string(c,argc,a,1,to)) { state->set_rig_error("mirrorBranch requires non-empty string from and to names"); return JS_UNDEFINED; }
    if (argc <= 2 || !JS_IsObject(a[2])) { state->set_rig_error("mirrorBranch requires an options object"); return JS_UNDEFINED; }
    JSValue axis=JS_GetPropertyStr(c,a[2],"axis"); std::string axis_name; const bool valid_axis=JS_IsString(axis) && rig_nonempty_string(c,1,&axis,0,axis_name); JS_FreeValue(c,axis);
    const int plane=valid_axis && axis_name=="x"?0:(valid_axis && axis_name=="y"?1:(valid_axis && axis_name=="z"?2:-1)); if (plane < 0) { state->set_rig_error("mirrorBranch requires axis 'x', 'y', or 'z'"); return JS_UNDEFINED; }
    std::string rename_from,rename_to; std::map<std::string,std::string> names;
    JSValue rename=JS_GetPropertyStr(c,a[2],"rename");
    if (!JS_IsUndefined(rename)) { if (!JS_IsObject(rename)) { JS_FreeValue(c,rename); state->set_rig_error("mirrorBranch requires string rename.from and rename.to"); return JS_UNDEFINED; } JSValue f=JS_GetPropertyStr(c,rename,"from"), t=JS_GetPropertyStr(c,rename,"to"); const bool valid=rig_nonempty_string(c,1,&f,0,rename_from) && rig_nonempty_string(c,1,&t,0,rename_to); JS_FreeValue(c,f); JS_FreeValue(c,t); if (!valid) { JS_FreeValue(c,rename); state->set_rig_error("mirrorBranch requires string rename.from and rename.to"); return JS_UNDEFINED; } } JS_FreeValue(c,rename);
    JSValue map=JS_GetPropertyStr(c,a[2],"map");
    if (!JS_IsUndefined(map)) { if (!JS_IsObject(map)) { JS_FreeValue(c,map); state->set_rig_error("mirrorBranch requires an object map with string values"); return JS_UNDEFINED; } JSPropertyEnum* props=nullptr; uint32_t count=0; const int listed=JS_GetOwnPropertyNames(c,&props,&count,map,JS_GPN_STRING_MASK); if (listed != 0) { JS_FreeValue(c,map); state->set_rig_error("mirrorBranch requires an object map with string values"); return JS_UNDEFINED; } bool valid_map=true; for (uint32_t i=0;i<count;++i) { const char* key=JS_AtomToCString(c,props[i].atom); JSValue value=JS_GetProperty(c,map,props[i].atom); std::string mapped; const bool valid=key && *key && rig_nonempty_string(c,1,&value,0,mapped); if (valid) names[key]=mapped; else valid_map=false; if (key) JS_FreeCString(c,key); JS_FreeValue(c,value); JS_FreeAtom(c,props[i].atom); } js_free(c,props); if (!valid_map) { JS_FreeValue(c,map); state->set_rig_error("mirrorBranch requires an object map with string values"); return JS_UNDEFINED; } } JS_FreeValue(c,map);
    state->rig_mirror_branch(from,to,plane,rename_from,rename_to,names); return JS_UNDEFINED;
}
static JSValue j_endRig(JSContext* c, JSValueConst, int n, JSValueConst* a) { rig_source(c,n,a,"endRig"); state_of(c)->end_rig(); return JS_UNDEFINED; }
static bool binding_joints(JSContext* c, JSValueConst value, std::vector<std::string>& out) {
    if (JS_IsUndefined(value)) return true;
    if (!JS_IsArray(value)) return false;
    JSValue length_value=JS_GetPropertyStr(c,value,"length"); uint32_t length=0;
    const bool length_ok=JS_ToUint32(c,&length,length_value)>=0; JS_FreeValue(c,length_value);
    if (!length_ok) return false;
    out.clear(); out.reserve(length);
    for (uint32_t index=0; index<length; ++index) {
        JSValue item=JS_GetPropertyUint32(c,value,index); std::string joint;
        const bool ok=anim_string(c,item,joint); JS_FreeValue(c,item);
        if (!ok) return false;
        out.push_back(std::move(joint));
    }
    return true;
}
static bool binding_number_property(JSContext* c, JSValueConst options, const char* key, float& out) {
    JSValue value=JS_GetPropertyStr(c,options,key);
    if (JS_IsUndefined(value)) { JS_FreeValue(c,value); return true; }
    double number=0.0; const bool ok=JS_IsNumber(value)&&JS_ToFloat64(c,&number,value)>=0&&std::isfinite(number);
    JS_FreeValue(c,value); if (ok) out=static_cast<float>(number); return ok;
}
// v1 names are accepted as single, documented aliases. Supplying both names
// is an authoring error: choosing a precedence would make source ambiguous.
static bool binding_number_alias(JSContext* c, JSValueConst options, const char* primary, const char* legacy, float& out) {
    JSValue current=JS_GetPropertyStr(c,options,primary), old=JS_GetPropertyStr(c,options,legacy);
    const bool has_current=!JS_IsUndefined(current), has_old=!JS_IsUndefined(old);
    if (has_current && has_old) { JS_FreeValue(c,current); JS_FreeValue(c,old); return false; }
    JSValue chosen=has_current?current:old; double number=0.0;
    const bool ok=JS_IsUndefined(chosen)||(JS_IsNumber(chosen)&&JS_ToFloat64(c,&number,chosen)>=0&&std::isfinite(number));
    if (ok && !JS_IsUndefined(chosen)) out=static_cast<float>(number);
    JS_FreeValue(c,current); JS_FreeValue(c,old); return ok;
}
static bool binding_bool_property(JSContext* c, JSValueConst options, const char* key, bool& out) {
    JSValue value=JS_GetPropertyStr(c,options,key);
    if (JS_IsUndefined(value)) { JS_FreeValue(c,value); return true; }
    const bool ok=JS_IsBool(value); if (ok) out=JS_ToBool(c,value)>0; JS_FreeValue(c,value); return ok;
}
static JSValue j_skin(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    rig_source(c,n,a,"skin"); const int argc=rig_user_argc(n); std::string name;
    if (argc < 1 || !rig_nonempty_string(c,argc,a,0,name)) { state_of(c)->set_rig_error("skin requires a non-empty name"); return JS_UNDEFINED; }
    if (argc > 2 || (argc == 2 && !JS_IsObject(a[1]))) { state_of(c)->set_rig_error("skin options must be an object"); return JS_UNDEFINED; }
    std::vector<std::string> joints; float radius_scale=1.0f, falloff=1.0f, spacing=0.1f; bool generate=true;
    if (argc == 2) {
        JSValue value=JS_GetPropertyStr(c,a[1],"joints"); const bool joints_ok=binding_joints(c,value,joints); JS_FreeValue(c,value);
        if (!joints_ok || !binding_number_property(c,a[1],"radiusScale",radius_scale) || !binding_number_alias(c,a[1],"falloffScale","falloff",falloff) || !binding_number_alias(c,a[1],"voxelSize","spacing",spacing) || !binding_bool_property(c,a[1],"generate",generate)) { state_of(c)->set_rig_error("skin options require string joints and finite radiusScale/falloffScale/voxelSize values (falloff and spacing are aliases)"); return JS_UNDEFINED; }
    }
    state_of(c)->rig_skin(name,joints,radius_scale,falloff,generate,spacing); return JS_UNDEFINED;
}
static JSValue j_segments(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    rig_source(c,n,a,"segments"); const int argc=rig_user_argc(n); std::string name;
    if (argc < 1 || !rig_nonempty_string(c,argc,a,0,name)) { state_of(c)->set_rig_error("segments requires a non-empty name"); return JS_UNDEFINED; }
    if (argc > 2 || (argc == 2 && !JS_IsObject(a[1]))) { state_of(c)->set_rig_error("segments options must be an object"); return JS_UNDEFINED; }
    std::vector<std::string> joints; bool decorative=false; matter::AnimationTransform offset{};
    if (argc == 2) { JSValue value=JS_GetPropertyStr(c,a[1],"joints"); const bool joints_ok=binding_joints(c,value,joints); JS_FreeValue(c,value); bool transform_ok=true; value=JS_GetPropertyStr(c,a[1],"offset"); if(!JS_IsUndefined(value))offset=socket_transform(c,value,&transform_ok); JS_FreeValue(c,value); if (!joints_ok || !binding_bool_property(c,a[1],"decorative",decorative) || !transform_ok) { state_of(c)->set_rig_error("segments options require string joints, a boolean decorative flag, and a finite offset transform"); return JS_UNDEFINED; } }
    state_of(c)->rig_segments(name,joints,decorative,offset); return JS_UNDEFINED;
}
static JSValue j_attach(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    rig_source(c,n,a,"attach"); const int argc=rig_user_argc(n); std::string name,socket,module;
    if (argc < 3 || argc > 4 || !rig_nonempty_string(c,argc,a,0,name) || !rig_nonempty_string(c,argc,a,1,socket) || !rig_nonempty_string(c,argc,a,2,module)) { state_of(c)->set_rig_error("attach requires name, socket, and child module strings"); return JS_UNDEFINED; }
    bool ok=true; matter::AnimationTransform local{}; if (argc == 4) local=socket_transform(c,a[3],&ok);
    if (!ok) { state_of(c)->set_rig_error("attach transform must be a finite transform object"); return JS_UNDEFINED; }
    state_of(c)->rig_attach(name,socket,module,local); return JS_UNDEFINED;
}
static JSValue j_bind_geometry(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    rig_source(c,n,a,"bind"); const int argc=rig_user_argc(n); std::string name;
    DslState* state=state_of(c);
    if (argc != 2 || !rig_nonempty_string(c,argc,a,0,name) || !JS_IsFunction(c,a[1])) {
        state->set_rig_error("bind requires a skin binding name and callback"); return JS_UNDEFINED;
    }
    if (!state->begin_binding_scope(name)) return JS_UNDEFINED;
    JSValue result=JS_Call(c,a[1],JS_UNDEFINED,0,nullptr);
    const bool callback_failed=JS_IsException(result);
    JS_FreeValue(c,result);
    if (callback_failed) { state->cancel_binding_scope(); state->set_rig_error("bind callback failed"); return JS_UNDEFINED; }
    if (state->has_error()) { state->cancel_binding_scope(); return JS_UNDEFINED; }
    if (!state->end_binding_scope()) state->cancel_binding_scope();
    return JS_UNDEFINED;
}
static JSValue j_sphere(JSContext* c, JSValueConst, int, JSValueConst* a){
    state_of(c)->sphere({(float)argd(c,a[0]),(float)argd(c,a[1]),(float)argd(c,a[2])},(float)argd(c,a[3]),CsgOp::Union); return JS_UNDEFINED; }
static JSValue j_box(JSContext* c, JSValueConst, int, JSValueConst* a){
    state_of(c)->box({(float)argd(c,a[0]),(float)argd(c,a[1]),(float)argd(c,a[2])},
                     {(float)argd(c,a[3]),(float)argd(c,a[4]),(float)argd(c,a[5])},CsgOp::Union); return JS_UNDEFINED; }
static JSValue j_op(JSContext* c, JSValueConst, int, JSValueConst* a){
    int32_t k=0; JS_ToInt32(c,&k,a[0]); state_of(c)->set_last_op((CsgOp)k); return JS_UNDEFINED; }
static JSValue j_smoothing(JSContext* c, JSValueConst, int, JSValueConst* a){ state_of(c)->smoothing((float)argd(c,a[0])); return JS_UNDEFINED; }

static JSValue j_raycast(JSContext* c, JSValueConst, int, JSValueConst* a){
    Vector3 hit{}, nrm{};
    bool ok = state_of(c)->raycast(
        {(float)argd(c,a[0]),(float)argd(c,a[1]),(float)argd(c,a[2])},
        {(float)argd(c,a[3]),(float)argd(c,a[4]),(float)argd(c,a[5])},
        hit, nrm);
    if (!ok) return JS_NULL;   // miss OR fail-closed error (bake fails anyway)
    JSValue pt = JS_NewArray(c);
    JS_SetPropertyUint32(c, pt, 0, JS_NewFloat64(c, hit.x));
    JS_SetPropertyUint32(c, pt, 1, JS_NewFloat64(c, hit.y));
    JS_SetPropertyUint32(c, pt, 2, JS_NewFloat64(c, hit.z));
    JSValue nm = JS_NewArray(c);
    JS_SetPropertyUint32(c, nm, 0, JS_NewFloat64(c, nrm.x));
    JS_SetPropertyUint32(c, nm, 1, JS_NewFloat64(c, nrm.y));
    JS_SetPropertyUint32(c, nm, 2, JS_NewFloat64(c, nrm.z));
    JSValue obj = JS_NewObject(c);
    JS_SetPropertyStr(c, obj, "point", pt);
    JS_SetPropertyStr(c, obj, "normal", nm);
    return obj;
}

static JSValue j_beginModifier(JSContext* c, JSValueConst, int, JSValueConst*) {
    state_of(c)->begin_modifier_region(); return JS_UNDEFINED; }

// Optional numeric field: returns true and fills `out` iff present (non-null).
static bool opt_num(JSContext* c, JSValueConst obj, const char* k, double& out) {
    JSValue v = JS_GetPropertyStr(c, obj, k);
    const bool has = !JS_IsUndefined(v) && !JS_IsNull(v);
    if (has) JS_ToFloat64(c, &out, v);
    JS_FreeValue(c, v);
    return has;
}

// endModifier(list): an Array of one-key objects in execution order, e.g.
//   [{ smooth: { iterations: 2 } }, { retopo: {...} }, { simplify: 0.3 }]
// Shorthand: { simplify: 0.3 } (bare number).
static JSValue j_endModifier(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    DslState* st = state_of(c);
    if (n < 1 || !JS_IsArray(a[0])) {
        st->set_error("endModifier: expected an array of modifier entries");
        return JS_UNDEFINED;
    }
    JSValue lenv = JS_GetPropertyStr(c, a[0], "length");
    uint32_t len = 0; JS_ToUint32(c, &len, lenv); JS_FreeValue(c, lenv);

    std::vector<ModifierSpec> stack;
    for (uint32_t i = 0; i < len && !st->has_error(); ++i) {
        JSValue entry = JS_GetPropertyUint32(c, a[0], i);
        JSPropertyEnum* props = nullptr;
        uint32_t pcount = 0;
        if (!JS_IsObject(entry) ||
            JS_GetOwnPropertyNames(c, &props, &pcount, entry,
                                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
            st->set_error("endModifier: each entry must be an object like { smooth: {...} }");
            JS_FreeValue(c, entry);
            break;
        }
        if (pcount != 1) {
            st->set_error("endModifier: each entry must have exactly one key (the modifier name)");
        } else {
            const char* key = JS_AtomToCString(c, props[0].atom);
            JSValue val = JS_GetProperty(c, entry, props[0].atom);
            ModifierSpec spec{};
            double d = 0.0;
            if (key && !std::strcmp(key, "simplify")) {
                spec.kind = ModifierKind::Simplify;
                if (JS_IsNumber(val)) d = argd(c, val);          // shorthand { simplify: 0.3 }
                else if (!opt_num(c, val, "ratio", d)) d = 0.0;  // { simplify: { ratio: 0.3 } }
                if (!(d > 0.0 && d <= 1.0))
                    st->set_error("endModifier: simplify ratio must be in (0, 1]");
                spec.ratio = (float)d;
            } else if (key && !std::strcmp(key, "smooth")) {
                spec.kind = ModifierKind::Smooth;
                if (!JS_IsObject(val)) {
                    st->set_error("endModifier: smooth params must be an object");
                } else {
                    if (opt_num(c, val, "iterations", d)) spec.iterations = (int)d;
                    if (opt_num(c, val, "lambda", d))     spec.lambda = (float)d;
                    if (opt_num(c, val, "mu", d))         spec.mu = (float)d;
                    if (spec.iterations < 1 || !(spec.lambda > 0.0f) || !(spec.mu < 0.0f))
                        st->set_error("endModifier: smooth params out of range "
                                      "(iterations>=1, lambda>0, mu<0)");
                }
            } else if (key && !std::strcmp(key, "retopo")) {
                spec.kind = ModifierKind::Retopo;
                if (!JS_IsObject(val)) {
                    st->set_error("endModifier: retopo params must be an object");
                } else {
                    if (opt_num(c, val, "target_ratio", d))    spec.target_ratio = (float)d;
                    if (opt_num(c, val, "iterations", d))      spec.retopo_iterations = (int)d;
                    if (opt_num(c, val, "seed", d))            spec.seed = (uint32_t)d;
                    if (opt_num(c, val, "timeout_seconds", d)) spec.timeout_seconds = (int)d;
                    if (!(spec.target_ratio > 0.0f) || spec.retopo_iterations < 1 ||
                        spec.timeout_seconds < 1)
                        st->set_error("endModifier: retopo params out of range");
                }
            } else {
                st->set_error(std::string("endModifier: unknown modifier '") +
                              (key ? key : "?") + "'");
            }
            if (key) JS_FreeCString(c, key);
            JS_FreeValue(c, val);
            if (!st->has_error()) stack.push_back(spec);
        }
        for (uint32_t p = 0; p < pcount; ++p) JS_FreeAtom(c, props[p].atom);
        js_free(c, props);
        JS_FreeValue(c, entry);
    }
    if (!st->has_error()) st->end_modifier_region(std::move(stack));
    return JS_UNDEFINED;
}

static JSValue j_placeChild(JSContext* c, JSValueConst, int n, JSValueConst* a){
    const char* m = JS_ToCString(c, a[0]);
    if (!m) return JS_UNDEFINED;
    // Parse optional third-argument options object { instanced, inlineBelowPx }.
    bool instanced = false;
    double inline_px = 0.0;
    if (n > 2 && JS_IsObject(a[2])) {
        JSValue vi = JS_GetPropertyStr(c, a[2], "instanced");
        instanced = JS_ToBool(c, vi) > 0;
        JS_FreeValue(c, vi);
        JSValue vp = JS_GetPropertyStr(c, a[2], "inlineBelowPx");
        if (!JS_IsUndefined(vp) && !JS_IsNull(vp)) JS_ToFloat64(c, &inline_px, vp);
        JS_FreeValue(c, vp);
        if (instanced && inline_px <= 0.0) inline_px = 64.0;   // engine default
    }
    // G6: optional params (a plain JS object/array) -> canonical JSON bytes folded
    // into the child's resolved hash so parametric children dedup. Normalize via
    // params_from_json -> params_to_json (sorted keys, %.17g numbers) so the lookup
    // key matches the name2hash keys the host builds with params_to_json(kid.params).
    // Without normalization, JS_JSONStringify's ES key order and shortest-repr numbers
    // differ from the canonical format, causing composite key misses and a silent
    // bare-module fallback that collapsed all parametric placements to one hash.
    // No params (undefined/null) = bare-module lookup (unchanged behavior).
    if (n > 1 && !JS_IsUndefined(a[1]) && !JS_IsNull(a[1])) {
        JSValue js = JS_JSONStringify(c, a[1], JS_UNDEFINED, JS_UNDEFINED);
        if (!JS_IsException(js)) {
            size_t len = 0;
            const char* s = JS_ToCStringLen(c, &len, js);
            if (s) {
                std::string normalized = normalize_params_json(s, len);
                JS_FreeCString(c, s);
                if (!normalized.empty() && normalized != "{}") {
                    state_of(c)->placeChild(m, normalized.c_str(), normalized.size(), instanced, (float)inline_px);
                } else {
                    state_of(c)->placeChild(m, nullptr, 0, instanced, (float)inline_px);
                }
            } else {
                state_of(c)->placeChild(m, nullptr, 0, instanced, (float)inline_px);
            }
        } else {
            state_of(c)->placeChild(m, nullptr, 0, instanced, (float)inline_px);
        }
        JS_FreeValue(c, js);
    } else {
        state_of(c)->placeChild(m, nullptr, 0, instanced, (float)inline_px);
    }
    JS_FreeCString(c, m);
    return JS_UNDEFINED; }

static JSValue j_beginShape(JSContext* c, JSValueConst, int, JSValueConst* a){
    int32_t mode=0; JS_ToInt32(c,&mode,a[0]); state_of(c)->beginShape(mode); return JS_UNDEFINED; }
static JSValue j_vertex(JSContext* c, JSValueConst, int, JSValueConst* a){
    state_of(c)->vertex((float)argd(c,a[0]),(float)argd(c,a[1]),(float)argd(c,a[2])); return JS_UNDEFINED; }
static JSValue j_endShape(JSContext* c, JSValueConst, int, JSValueConst*){ state_of(c)->endShape(); return JS_UNDEFINED; }
static JSValue j_beginContour(JSContext* c, JSValueConst, int, JSValueConst*){ state_of(c)->beginContour(); return JS_UNDEFINED; }
static JSValue j_endContour(JSContext* c, JSValueConst, int, JSValueConst*){ state_of(c)->endContour(); return JS_UNDEFINED; }
static JSValue j_joinType(JSContext* c, JSValueConst, int, JSValueConst* a){
    int32_t k=0; JS_ToInt32(c,&k,a[0]); state_of(c)->joinType((int)k); return JS_UNDEFINED; }
static JSValue j_extrude(JSContext* c, JSValueConst, int, JSValueConst* a){
    // extrude(path): path is a JS array of points, each point a 3-element
    // [x,y,z] array. Flatten into 3*n floats for DslState::extrude.
    JSValue lenv = JS_GetPropertyStr(c, a[0], "length");
    int32_t n = 0; JS_ToInt32(c, &n, lenv); JS_FreeValue(c, lenv);
    if (n < 2) { state_of(c)->extrude(nullptr, 0); return JS_UNDEFINED; }
    std::vector<float> flat; flat.reserve((size_t)n * 3);
    for (int32_t i = 0; i < n; ++i) {
        JSValue pt = JS_GetPropertyUint32(c, a[0], (uint32_t)i);
        for (int j = 0; j < 3; ++j) {
            JSValue e = JS_GetPropertyUint32(c, pt, (uint32_t)j);
            flat.push_back((float)argd(c, e));
            JS_FreeValue(c, e);
        }
        JS_FreeValue(c, pt);
    }
    state_of(c)->extrude(flat.data(), n);
    return JS_UNDEFINED; }
static JSValue j_position(JSContext* c, JSValueConst, int, JSValueConst*){
    Vector3 p = state_of(c)->position();
    JSValue arr = JS_NewArray(c);
    JS_SetPropertyUint32(c, arr, 0, JS_NewFloat64(c, p.x));
    JS_SetPropertyUint32(c, arr, 1, JS_NewFloat64(c, p.y));
    JS_SetPropertyUint32(c, arr, 2, JS_NewFloat64(c, p.z));
    return arr; }
static JSValue j_line(JSContext* c, JSValueConst, int, JSValueConst* a){
    state_of(c)->line((float)argd(c,a[0]),(float)argd(c,a[1]),(float)argd(c,a[2]),
                      (float)argd(c,a[3]),(float)argd(c,a[4]),(float)argd(c,a[5]),
                      (float)argd(c,a[6]),(float)argd(c,a[7])); return JS_UNDEFINED; }
// Round primitives (Phase 4). a,b are segment endpoints; r0/r1 end radii.
// Voxel-session => SDF brush; None => clean error (mesh emitters land in Phase 5).
static JSValue j_capsule(JSContext* c, JSValueConst, int, JSValueConst* a){
    state_of(c)->capsule({(float)argd(c,a[0]),(float)argd(c,a[1]),(float)argd(c,a[2])},
                         {(float)argd(c,a[3]),(float)argd(c,a[4]),(float)argd(c,a[5])},
                         (float)argd(c,a[6]), CsgOp::Union); return JS_UNDEFINED; }
static JSValue j_cylinder(JSContext* c, JSValueConst, int, JSValueConst* a){
    state_of(c)->cylinder({(float)argd(c,a[0]),(float)argd(c,a[1]),(float)argd(c,a[2])},
                          {(float)argd(c,a[3]),(float)argd(c,a[4]),(float)argd(c,a[5])},
                          (float)argd(c,a[6]), CsgOp::Union); return JS_UNDEFINED; }
static JSValue j_cone(JSContext* c, JSValueConst, int, JSValueConst* a){
    state_of(c)->cone({(float)argd(c,a[0]),(float)argd(c,a[1]),(float)argd(c,a[2])},
                      {(float)argd(c,a[3]),(float)argd(c,a[4]),(float)argd(c,a[5])},
                      (float)argd(c,a[6]), (float)argd(c,a[7]), CsgOp::Union); return JS_UNDEFINED; }

// Seeded Math.random: draws from the bake's DslState Rng (seeded by the host
// before build()). Deterministic and process-entropy-free so the resolved-hash
// <-> bytes contract holds. Falls back to 0.0 if no Rng is installed (which
// keeps the bake deterministic rather than reaching for engine entropy).
static JSValue j_random(JSContext* c, JSValueConst, int, JSValueConst*) {
    DslState* st = state_of(c);
    double d = (st && st->rng()) ? st->rng()->next_unit() : 0.0;
    return JS_NewFloat64(c, d);
}

// ---------------------------------------------------------------------------
// Tileset bindings (__dsl_ts_*)
// ---------------------------------------------------------------------------
static tileset::TilesetState* ts_of(JSContext* c) {
    dsl::DslState* s = state_of(c);
    return s ? s->tileset() : nullptr;
}

// Uniform random unit quaternion (Box-Muller). Draws 4 normal floats, normalizes.
// Guard: retry if norm is degenerate (astronomically rare).
static void random_unit_quat(dsl::Rng& r, float q[4]) {
    for (;;) {
        float g[4];
        for (int i = 0; i < 4; i += 2) {
            float u1 = (float)r.next_unit(); if (u1 < 1e-7f) u1 = 1e-7f;
            float u2 = (float)r.next_unit();
            float m = std::sqrt(-2.0f * std::log(u1));
            g[i]     = m * std::cos(6.2831853f * u2);
            g[i + 1] = m * std::sin(6.2831853f * u2);
        }
        float n = std::sqrt(g[0]*g[0] + g[1]*g[1] + g[2]*g[2] + g[3]*g[3]);
        if (n > 1e-6f) { q[0]=g[0]/n; q[1]=g[1]/n; q[2]=g[2]/n; q[3]=g[3]/n; return; }
    }
}

// Params-fn `r` helper: int(n) and float(a,b) drawing from ts->param_rng.
static JSValue j_ts_rng_int(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    tileset::TilesetState* ts = ts_of(c);
    if (!ts || !ts->param_rng) return JS_NewInt32(c, 0);
    int32_t nn = 1; if (n > 0) JS_ToInt32(c, &nn, a[0]);
    if (nn <= 1) return JS_NewInt32(c, 0);
    uint64_t v = ts->param_rng->next_u64();
    return JS_NewInt32(c, (int32_t)(v % (uint64_t)nn));
}
static JSValue j_ts_rng_float(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    tileset::TilesetState* ts = ts_of(c);
    if (!ts || !ts->param_rng) return JS_NewFloat64(c, 0.0);
    double lo = 0.0, hi = 1.0;
    if (n > 0) JS_ToFloat64(c, &lo, a[0]);
    if (n > 1) JS_ToFloat64(c, &hi, a[1]);
    double u = ts->param_rng->next_unit();
    return JS_NewFloat64(c, lo + u * (hi - lo));
}

static JSValue j_ts_tile(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    tileset::TilesetState* ts = ts_of(c);
    if (!ts) { state_of(c)->set_error("tileset verb outside Tileset root"); return JS_UNDEFINED; }
    if (ts->spec.tile_called) { ts->set_error("tile() called twice"); return JS_UNDEFINED; }
    tileset::TileConfig& cfg = ts->spec.cfg;
    double v;
    if (n > 0 && !JS_IsUndefined(a[0]) && !JS_ToFloat64(c, &v, a[0])) cfg.size = (float)v;
    if (n > 1 && !JS_IsUndefined(a[1]) && !JS_ToFloat64(c, &v, a[1])) cfg.texels_per_meter = (int)v;
    if (n > 2 && !JS_IsUndefined(a[2]) && !JS_ToFloat64(c, &v, a[2])) cfg.seed = (uint64_t)(double)v;
    if (n > 3 && !JS_IsUndefined(a[3]) && !JS_ToFloat64(c, &v, a[3])) cfg.edge_strip_width = (float)v;
    if (n > 4 && !JS_IsUndefined(a[4]) && !JS_ToFloat64(c, &v, a[4])) cfg.corner_clear_radius = (float)v;
    if (cfg.size <= 0.0f) { ts->set_error("tile: size must be positive"); return JS_UNDEFINED; }
    if (cfg.texels_per_meter <= 0) { ts->set_error("tile: texelsPerMeter must be positive"); return JS_UNDEFINED; }
    if (cfg.edge_strip_width <= cfg.corner_clear_radius) { ts->set_error("tile(): edgeStripWidth must exceed cornerClearRadius"); return JS_UNDEFINED; }
    ts->spec.tile_called = true;
    return JS_UNDEFINED;
}

static JSValue j_ts_base(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    tileset::TilesetState* ts = ts_of(c);
    if (!ts) { state_of(c)->set_error("tileset verb outside Tileset root"); return JS_UNDEFINED; }
    if (!ts->spec.tile_called) { ts->set_error("base() before tile()"); return JS_UNDEFINED; }
    if (n < 2 || !JS_IsFunction(c, a[0])) { ts->set_error("base(fn, material): fn required"); return JS_UNDEFINED; }
    uint32_t mat = 0; JS_ToUint32(c, &mat, a[1]);

    tileset::BaseField& b = ts->spec.base;
    b.n = tileset::BaseField::kSamplesPerTile;
    b.cell = ts->spec.cfg.size / (float)b.n;
    b.material = mat;
    b.heights.assign((size_t)b.n * b.n, 0.0f);
    for (int z = 0; z < b.n; ++z) {
        for (int x = 0; x < b.n; ++x) {
            JSValue args[2] = { JS_NewFloat64(c, x * b.cell), JS_NewFloat64(c, z * b.cell) };
            JSValue rv = JS_Call(c, a[0], JS_UNDEFINED, 2, args);
            JS_FreeValue(c, args[0]); JS_FreeValue(c, args[1]);
            if (JS_IsException(rv)) { ts->set_error("base(): heightfield fn threw"); return JS_EXCEPTION; }
            double h = 0.0; JS_ToFloat64(c, &h, rv); JS_FreeValue(c, rv);
            b.heights[(size_t)z * b.n + x] = (float)h;
        }
    }
    b.set = true;
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// place_one_instance — shared per-point placement logic for j_ts_layer.
//
// Fills `out` with scale, pos[1], quat, and child_hash drawn from attr_rng.
// The x/z coordinates are NOT set here; the caller maps them after this call
// (strip orientation swap vs. interior pass-through differ by call site).
//
// orient: 0 = vertical strip (x=across, z=along),
//         1 = horizontal strip (x=along, z=across),
//        -1 = interior (x=pt.x, z=pt.z, no swap).
// The x/z assignment IS performed here so both call sites are unified.
//
// Does NOT free params_val or r_helper on error (caller owns them).
// Returns false and sets ts->has_error on any error; out is unmodified.
// ---------------------------------------------------------------------------
static bool place_one_instance(
    JSContext* c,
    tileset::TilesetState* ts,
    dsl::DslState* state,
    const std::string& module,
    const tileset::LayerSpec& layer,
    bool params_is_fn,
    uint64_t fixed_hash,
    JSValueConst params_val,
    JSValueConst r_helper,
    dsl::Rng& attr_rng,
    const tileset::Point2& pt,
    int orient,
    tileset::Placement& out)
{
    tileset::Placement p{};

    // scale drawn first.
    // NOTE: if scale_range transitions between degenerate ([a,a]) and
    // non-degenerate, the RNG draw is added or removed here, shifting
    // every subsequent placement attribute (y, quat, params) in the stream.
    if (layer.scale_range[0] == layer.scale_range[1]) {
        p.scale = layer.scale_range[0];
    } else {
        p.scale = layer.scale_range[0] +
                  (float)attr_rng.next_unit() * (layer.scale_range[1] - layer.scale_range[0]);
    }

    // y / quat
    if (layer.physics) {
        p.pos[1] = layer.drop_h[0] +
                   (float)attr_rng.next_unit() * (layer.drop_h[1] - layer.drop_h[0]);
        random_unit_quat(attr_rng, p.quat);
    } else {
        p.pos[1] = 0.0f;
        float angle = (float)attr_rng.next_unit() * 6.2831853f;
        float half = angle * 0.5f;
        p.quat[0] = 0.0f;
        p.quat[1] = std::sin(half);
        p.quat[2] = 0.0f;
        p.quat[3] = std::cos(half);
    }

    // params
    if (params_is_fn) {
        ts->param_rng = &attr_rng;
        JSValue ret = JS_Call(c, params_val, JS_UNDEFINED, 1, &r_helper);
        ts->param_rng = nullptr;
        if (JS_IsException(ret)) {
            JS_FreeValue(c, ret);
            ts->set_error("layer('" + module + "'): params fn threw");
            return false;
        }
        JSValue js = JS_JSONStringify(c, ret, JS_UNDEFINED, JS_UNDEFINED);
        JS_FreeValue(c, ret);
        std::string pjson;
        if (!JS_IsException(js)) {
            size_t len = 0;
            const char* s = JS_ToCStringLen(c, &len, js);
            if (s) { pjson.assign(s, len); JS_FreeCString(c, s); }
        }
        JS_FreeValue(c, js);
        // Normalize: round-trip through params_from_json -> params_to_json so the
        // composite key matches name2hash's canonical bytes (sorted keys, %.17g).
        if (!pjson.empty() && pjson != "{}") {
            pjson = normalize_params_json(pjson.c_str(), pjson.size());
        }
        uint64_t h = 0;
        // Fail-closed: if fn returned a non-trivial params object, the composite
        // key (module\x1f<params>) must be explicitly declared in static requires.
        // Trivial (empty/"{}"): fall through to plain-module lookup so that a fn
        // that returns {} for a plain-declared module still resolves correctly.
        bool has_real_params = !pjson.empty() && pjson != "{}";
        if (has_real_params &&
            !state->has_composite_child_key(module, pjson.c_str(), pjson.size())) {
            ts->set_error("layer('" + module + "'): params variant not declared in static requires");
            return false;
        }
        if (!state->lookup_child_hash(module, has_real_params ? pjson.c_str() : nullptr,
                                      has_real_params ? pjson.size() : 0, h)) {
            ts->set_error("layer('" + module + "'): params variant not declared in static requires");
            return false;
        }
        p.child_hash = h;
    } else {
        p.child_hash = fixed_hash;
    }

    // Map coordinates: strips swap x/z based on orientation; interior is direct.
    if (orient == 0) {
        p.pos[0] = pt.x; p.pos[2] = pt.z;  // vertical: x=across, z=along
    } else if (orient == 1) {
        p.pos[0] = pt.z; p.pos[2] = pt.x;  // horizontal: x=along, z=across
    } else {
        p.pos[0] = pt.x; p.pos[2] = pt.z;  // interior: direct
    }

    out = p;
    return true;
}

static JSValue j_ts_layer(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    tileset::TilesetState* ts = ts_of(c);
    if (!ts) { state_of(c)->set_error("tileset verb outside Tileset root"); return JS_UNDEFINED; }
    if (ts->has_error) return JS_UNDEFINED;
    dsl::DslState* state = state_of(c);

    // -- Argument 0: module name --
    if (n < 1 || JS_IsUndefined(a[0])) {
        ts->set_error("layer: module name required"); return JS_UNDEFINED;
    }
    const char* mstr = JS_ToCString(c, a[0]);
    if (!mstr) { ts->set_error("layer: module name required"); return JS_UNDEFINED; }
    std::string module(mstr);
    JS_FreeCString(c, mstr);

    // -- tile() must have been called first --
    if (!ts->spec.tile_called) {
        ts->set_error("layer('" + module + "'): tile() must be called before layer()");
        return JS_UNDEFINED;
    }

    // -- Argument 1: opts object --
    JSValue opts = (n > 1 && !JS_IsUndefined(a[1])) ? a[1] : JS_UNDEFINED;

    // Helper to read a named float property from opts.
    auto get_float = [&](const char* key, bool* found) -> double {
        if (JS_IsUndefined(opts)) { if (found) *found=false; return 0.0; }
        JSValue v = JS_GetPropertyStr(c, opts, key);
        if (JS_IsUndefined(v) || JS_IsNull(v)) { JS_FreeValue(c,v); if (found) *found=false; return 0.0; }
        double d = 0.0; JS_ToFloat64(c, &d, v); JS_FreeValue(c,v);
        if (found) *found=true; return d;
    };
    auto get_bool = [&](const char* key, bool def) -> bool {
        if (JS_IsUndefined(opts)) return def;
        JSValue v = JS_GetPropertyStr(c, opts, key);
        if (JS_IsUndefined(v) || JS_IsNull(v)) { JS_FreeValue(c,v); return def; }
        bool b = JS_ToBool(c, v) != 0; JS_FreeValue(c,v); return b;
    };
    auto get_str = [&](const char* key) -> std::string {
        if (JS_IsUndefined(opts)) return "";
        JSValue v = JS_GetPropertyStr(c, opts, key);
        if (!JS_IsString(v)) { JS_FreeValue(c,v); return ""; }
        const char* s = JS_ToCString(c, v); JS_FreeValue(c,v);
        if (!s) return "";
        std::string r(s); JS_FreeCString(c, s); return r;
    };
    // Read [min,max] array property.
    auto get_range = [&](const char* key, float def0, float def1, float out[2]) {
        out[0]=def0; out[1]=def1;
        if (JS_IsUndefined(opts)) return;
        JSValue v = JS_GetPropertyStr(c, opts, key);
        if (!JS_IsArray(v)) { JS_FreeValue(c,v); return; }
        JSValue e0=JS_GetPropertyUint32(c,v,0); JSValue e1=JS_GetPropertyUint32(c,v,1);
        double d0=def0, d1=def1;
        JS_ToFloat64(c,&d0,e0); JS_ToFloat64(c,&d1,e1);
        out[0]=(float)d0; out[1]=(float)d1;
        JS_FreeValue(c,e0); JS_FreeValue(c,e1); JS_FreeValue(c,v);
    };

    // -- Parse density (required) --
    bool density_found = false;
    double density_val = get_float("density", &density_found);
    if (!density_found || density_val <= 0.0) {
        ts->set_error("layer('" + module + "'): density required");
        return JS_UNDEFINED;
    }

    // -- Parse remaining opts with defaults --
    tileset::LayerSpec layer;
    layer.module = module;
    layer.density = (float)density_val;

    // placement: 'uniform'(0) / 'poisson'(1) / 'cluster'(2).
    // Fail-closed: if the property is present and not a string, error rather than
    // silently falling back to uniform (e.g. placement:42 must not compile).
    {
        layer.placement_kind = 0;   // default: uniform
        if (!JS_IsUndefined(opts)) {
            JSValue pv = JS_GetPropertyStr(c, opts, "placement");
            bool has_placement = !JS_IsUndefined(pv) && !JS_IsNull(pv);
            if (has_placement && !JS_IsString(pv)) {
                JS_FreeValue(c, pv);
                ts->set_error("layer('" + module + "'): placement must be a string");
                return JS_UNDEFINED;
            }
            if (has_placement) {
                const char* ps = JS_ToCString(c, pv);
                std::string pk(ps ? ps : "");
                if (ps) JS_FreeCString(c, ps);
                JS_FreeValue(c, pv);
                if (pk.empty() || pk == "uniform") layer.placement_kind = 0;
                else if (pk == "poisson")          layer.placement_kind = 1;
                else if (pk == "cluster")          layer.placement_kind = 2;
                else {
                    ts->set_error("layer('" + module + "'): unknown placement '" + pk + "'");
                    return JS_UNDEFINED;
                }
            } else {
                JS_FreeValue(c, pv);
            }
        }
    }

    layer.physics = get_bool("physics", true);
    layer.embed   = (float)get_float("embed", nullptr);
    get_range("dropHeight", 0.15f, 0.35f, layer.drop_h);
    get_range("scale",      1.0f,  1.0f,  layer.scale_range);
    layer.collider_override = get_str("collider");

    // -- Params: object, function, or absent --
    JSValue params_val = JS_UNDEFINED;
    bool params_is_fn = false;
    bool params_is_obj = false;
    if (!JS_IsUndefined(opts)) {
        params_val = JS_GetPropertyStr(c, opts, "params");
        if (JS_IsFunction(c, params_val))           params_is_fn  = true;
        else if (JS_IsObject(params_val))            params_is_obj = true;
        else { JS_FreeValue(c, params_val); params_val = JS_UNDEFINED; }
    }

    // If params is a static object, stringify and normalize once now.
    std::string static_params_json;
    if (params_is_obj) {
        JSValue js = JS_JSONStringify(c, params_val, JS_UNDEFINED, JS_UNDEFINED);
        if (!JS_IsException(js)) {
            size_t len=0; const char* s = JS_ToCStringLen(c, &len, js);
            if (s) {
                // Normalize: sorted keys + %.17g numbers so the composite key
                // matches name2hash's params_to_json bytes.
                static_params_json = normalize_params_json(s, len);
                JS_FreeCString(c,s);
            }
        }
        JS_FreeValue(c, js);
        JS_FreeValue(c, params_val); params_val = JS_UNDEFINED;
        // Validate: the composite key module\x1f params_json must be an explicit entry
        // (no plain-module fallback — an explicit params object must name a declared variant).
        if (!state->has_composite_child_key(module, static_params_json.c_str(),
                                            static_params_json.size())) {
            ts->set_error("layer('" + module + "'): params variant not declared in static requires");
            return JS_UNDEFINED;
        }
    } else if (!params_is_fn) {
        // No params: use empty string to fall back to plain module key.
        static_params_json = "";
    }

    // If no params, pre-resolve the hash once (plain module key).
    uint64_t fixed_hash = 0;
    bool has_fixed_hash = false;
    if (!params_is_fn) {
        has_fixed_hash = state->lookup_child_hash(
            module,
            static_params_json.empty() ? nullptr : static_params_json.c_str(),
            static_params_json.size(), fixed_hash);
        if (!has_fixed_hash) {
            ts->set_error("layer('" + module + "'): undeclared module (add to static requires)");
            JS_FreeValue(c, params_val);
            return JS_UNDEFINED;
        }
    }

    // -- Build the params-fn `r` helper object (shared across all placements) --
    JSValue r_helper = JS_UNDEFINED;
    if (params_is_fn) {
        r_helper = JS_NewObject(c);
        JSValue g = JS_GetGlobalObject(c);
        JSValue fn_int   = JS_GetPropertyStr(c, g, "__dsl_ts_rng_int");
        JSValue fn_float = JS_GetPropertyStr(c, g, "__dsl_ts_rng_float");
        JS_SetPropertyStr(c, r_helper, "int",   fn_int);
        JS_SetPropertyStr(c, r_helper, "float", fn_float);
        JS_FreeValue(c, g);
    }

    // -- Domain generation: 20 domains in fixed order --
    const tileset::TileConfig& cfg = ts->spec.cfg;
    const float w    = cfg.edge_strip_width;
    const float size = cfg.size;
    const float ccr  = cfg.corner_clear_radius;
    const uint64_t master_seed = cfg.seed;
    const uint32_t layer_index = (uint32_t)ts->spec.layers.size();  // index at entry
    tileset::PlacementKind pk = (tileset::PlacementKind)layer.placement_kind;

    // Domain ids: vStrip c0->0, c1->1, hStrip c0->2, c1->3, interior->4+tile
    // orientation 0 = vertical strips, orientation 1 = horizontal strips
    // Vertical strip: across = [-w, +w), along = [0, size). Map: pos={across, y, along}
    // Horizontal strip: across = [-w, +w), along = [0, size). Map: pos={along, y, across}
    // Corner clear disks: at along=0 and along=size (wrap), radius=ccr.

    for (int orient = 0; orient < 2; ++orient) {
        for (int color = 0; color < 2; ++color) {
            uint32_t domain_id = (uint32_t)(orient * 2 + color);
            uint64_t dom_seed = tileset::placement_seed(master_seed, layer_index, domain_id);
            uint64_t attr_seed = dom_seed ^ 0xA5A5A5A5A5A5A5A5ull;

            tileset::PlacementDomain dom;
            dom.x0 = -w; dom.x1 = w;
            dom.z0 = 0.0f; dom.z1 = size;
            dom.clear_disks = { {0.0f, 0.0f}, {0.0f, size} };
            dom.clear_radius = ccr;

            std::vector<tileset::Point2> pts = tileset::scatter(pk, dom, layer.density, dom_seed);

            dsl::Rng attr_rng(attr_seed);
            std::vector<tileset::Placement>& dest = layer.strip[orient][color];
            dest.reserve(pts.size());

            for (const auto& pt : pts) {
                tileset::Placement p{};
                if (!place_one_instance(c, ts, state, module, layer,
                                        params_is_fn, fixed_hash,
                                        params_val, r_helper,
                                        attr_rng, pt, orient, p)) {
                    JS_FreeValue(c, params_val);
                    JS_FreeValue(c, r_helper);
                    return JS_UNDEFINED;
                }
                dest.push_back(p);
            }
        }
    }

    // Interior tiles 0..15 (row*4+col)
    for (int tile = 0; tile < 16; ++tile) {
        uint32_t domain_id = 4u + (uint32_t)tile;
        uint64_t dom_seed = tileset::placement_seed(master_seed, layer_index, domain_id);
        uint64_t attr_seed = dom_seed ^ 0xA5A5A5A5A5A5A5A5ull;

        tileset::PlacementDomain dom;
        dom.x0 = w; dom.x1 = size - w;
        dom.z0 = w; dom.z1 = size - w;
        // No corner disks for interior (edgeStripWidth > cornerClearRadius enforced by tile())
        dom.clear_radius = 0.0f;

        std::vector<tileset::Point2> pts = tileset::scatter(pk, dom, layer.density, dom_seed);

        dsl::Rng attr_rng(attr_seed);
        std::vector<tileset::Placement>& dest = layer.interior[tile];
        dest.reserve(pts.size());

        for (const auto& pt : pts) {
            tileset::Placement p{};
            if (!place_one_instance(c, ts, state, module, layer,
                                    params_is_fn, fixed_hash,
                                    params_val, r_helper,
                                    attr_rng, pt, -1, p)) {
                JS_FreeValue(c, params_val);
                JS_FreeValue(c, r_helper);
                return JS_UNDEFINED;
            }
            dest.push_back(p);
        }
    }

    JS_FreeValue(c, params_val);
    JS_FreeValue(c, r_helper);

    // Push the completed LayerSpec (errors mid-generation leave no partial layer).
    ts->spec.layers.push_back(std::move(layer));
    return JS_UNDEFINED;
}

static JSValue j_ts_dropChild(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    tileset::TilesetState* ts = ts_of(c);
    if (!ts) { state_of(c)->set_error("tileset verb outside Tileset root"); return JS_UNDEFINED; }
    if (ts->has_error) return JS_UNDEFINED;
    dsl::DslState* state = state_of(c);

    // Get module name (arg 0).
    if (n < 1 || JS_IsUndefined(a[0])) {
        ts->set_error("dropChild: module name required"); return JS_UNDEFINED;
    }
    const char* m = JS_ToCString(c, a[0]);
    if (!m) { ts->set_error("dropChild: module name required"); return JS_UNDEFINED; }
    std::string module(m);
    JS_FreeCString(c, m);

    // Optional params (arg 1) — stringify and normalize like placeChild does.
    std::string params_str;
    if (n > 1 && !JS_IsUndefined(a[1]) && !JS_IsNull(a[1])) {
        JSValue js = JS_JSONStringify(c, a[1], JS_UNDEFINED, JS_UNDEFINED);
        if (!JS_IsException(js)) {
            size_t len = 0;
            const char* s = JS_ToCStringLen(c, &len, js);
            if (s) {
                params_str = normalize_params_json(s, len);
                JS_FreeCString(c, s);
            }
        }
        JS_FreeValue(c, js);
    }

    uint64_t hash = 0;
    if (!state->lookup_child_hash(module,
                                  params_str.empty() ? nullptr : params_str.c_str(),
                                  params_str.size(), hash)) {
        ts->set_error("dropChild('" + module + "'): undeclared module (add to static requires)");
        return JS_UNDEFINED;
    }

    tileset::DropChildRec rec{};
    rec.child_hash = hash;
    // Capture current transform stack top as row-major float[16].
    // Use the same matrix_to_row16 logic DslState::placeChild uses by accessing top().
    // We need the row-major layout; replicate the conversion inline here.
    Matrix mm = state->top();
    rec.transform[0]=mm.m0;  rec.transform[1]=mm.m4;  rec.transform[2]=mm.m8;  rec.transform[3]=mm.m12;
    rec.transform[4]=mm.m1;  rec.transform[5]=mm.m5;  rec.transform[6]=mm.m9;  rec.transform[7]=mm.m13;
    rec.transform[8]=mm.m2;  rec.transform[9]=mm.m6;  rec.transform[10]=mm.m10; rec.transform[11]=mm.m14;
    rec.transform[12]=mm.m3; rec.transform[13]=mm.m7; rec.transform[14]=mm.m11; rec.transform[15]=mm.m15;
    ts->spec.drops.push_back(rec);
    return JS_UNDEFINED;
}

static JSValue j_ts_variant(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    tileset::TilesetState* ts = ts_of(c);
    if (!ts) { state_of(c)->set_error("tileset verb outside Tileset root"); return JS_UNDEFINED; }
    if (ts->has_error) return JS_UNDEFINED;
    // variant() may only be called after tile().
    if (!ts->spec.tile_called) {
        ts->set_error("variant(): must be called after tile()");
        return JS_UNDEFINED;
    }
    // variant() may only be called once.
    if (ts->variant_called) {
        ts->set_error("variant(): called more than once");
        return JS_UNDEFINED;
    }
    ts->variant_called = true;
    // Register the hook fn: dup it and store the JSValue as raw bits.
    if (n < 1 || !JS_IsFunction(c, a[0])) {
        ts->set_error("variant(): argument must be a function");
        return JS_UNDEFINED;
    }
    JSValue fn = JS_DupValue(c, a[0]);
    static_assert(sizeof(fn) <= sizeof(ts->variant_fn_bits),
                  "JSValue too large for variant_fn_bits storage");
    std::memcpy(ts->variant_fn_bits, &fn, sizeof(fn));
    ts->variant_fn_set = true;
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// World query verbs: heightAt / slopeAt / moistureAt / biomeAt
// All fail loudly when no world field is bound.
// ---------------------------------------------------------------------------
static JSValue j_heightAt(JSContext* c, JSValueConst, int, JSValueConst* a) {
    DslState* st = state_of(c);
    const WorldBinding& w = st->world();
    if (!w.field) { st->set_error("heightAt: no world field bound"); return JS_UNDEFINED; }
    return JS_NewFloat64(c, w.field->height_at((float)argd(c, a[0]), (float)argd(c, a[1])));
}
static JSValue j_slopeAt(JSContext* c, JSValueConst, int, JSValueConst* a) {
    DslState* st = state_of(c);
    const WorldBinding& w = st->world();
    if (!w.field) { st->set_error("slopeAt: no world field bound"); return JS_UNDEFINED; }
    return JS_NewFloat64(c, w.field->slope_at((float)argd(c, a[0]), (float)argd(c, a[1])));
}
static JSValue j_moistureAt(JSContext* c, JSValueConst, int, JSValueConst* a) {
    DslState* st = state_of(c);
    const WorldBinding& w = st->world();
    if (!w.field) { st->set_error("moistureAt: no world field bound"); return JS_UNDEFINED; }
    return JS_NewFloat64(c, w.field->moisture_at((float)argd(c, a[0]), (float)argd(c, a[1])));
}
static JSValue j_biomeAt(JSContext* c, JSValueConst, int, JSValueConst* a) {
    DslState* st = state_of(c);
    const WorldBinding& w = st->world();
    if (!w.field) { st->set_error("biomeAt: no world field bound"); return JS_UNDEFINED; }
    using B = terrain_field::FieldRuntime;
    const char* s = "meadow";
    switch (w.field->biome_at((float)argd(c, a[0]), (float)argd(c, a[1]))) {
        case B::Ocean:     s = "ocean";     break;
        case B::Meadow:    s = "meadow";    break;
        case B::Foothills: s = "foothills"; break;
        case B::Mountains: s = "mountains"; break;
    }
    return JS_NewString(c, s);
}

// terrainVolume(tx, tz, rung, matArray)
// Meshes one sector of the bound terrain field using native surface-nets and
// pushes the result directly into the triangle buffer. matArray is an array of
// four material IDs [grass, dirt, rock, snow] indexed by the field's material_at.
// Fails loudly if no world binding is installed.
static JSValue j_terrainVolume(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    DslState* st = state_of(c);
    if (st->generating_animation()) {
        st->set_error("geometry authoring is forbidden during generate");
        return JS_UNDEFINED;
    }
    const WorldBinding& w = st->world();
    if (!w.field) {
        st->set_error("terrainVolume: no world bound — set BakeOptions.world before baking a terrain sector");
        return JS_UNDEFINED;
    }
    if (n < 3) { st->set_error("terrainVolume: requires (tx, tz, rung[, mats])"); return JS_UNDEFINED; }

    int64_t tx = 0, tz = 0;
    JS_ToInt64(c, &tx, a[0]);
    JS_ToInt64(c, &tz, a[1]);
    int32_t rung = 0;
    JS_ToInt32(c, &rung, a[2]);

    // Optional material array: up to 4 entries [grass, dirt, rock, snow].
    // Defaults to 0..3 if not supplied.
    uint32_t mat[4] = {0, 1, 2, 3};
    if (n >= 4 && JS_IsArray(a[3])) {
        for (int i = 0; i < 4; ++i) {
            JSValue v = JS_GetPropertyUint32(c, a[3], (uint32_t)i);
            if (!JS_IsUndefined(v)) {
                int32_t m = 0; JS_ToInt32(c, &m, v);
                mat[i] = (uint32_t)m;
            }
            JS_FreeValue(c, v);
        }
    }

    terrain_mesher::SectorMesh mesh;
    std::string err;
    if (!terrain_mesher::mesh_sector(*w.field, tx, tz, rung,
                                      w.sector_size, w.y_min, w.y_max, mesh, err)) {
        st->set_error("terrainVolume: " + err);
        return JS_UNDEFINED;
    }

    // Emit each material bucket with the mesher's gradient normals for smooth
    // terrain shading. Uses pushTerrainTriangle to bypass the face-normal
    // fallback in the standard beginShape/vertex/endShape path.
    for (const auto& bkt : mesh.buckets) {
        uint32_t mat_id = bkt.material < 4 ? mat[bkt.material] : mat[0];
        const size_t n_tris = bkt.positions.size() / 9;
        for (size_t t = 0; t < n_tris; ++t) {
            st->pushTerrainTriangle(&bkt.positions[t * 9],
                                    &bkt.normals[t * 9],
                                    (int)mat_id);
        }
    }
    return JS_UNDEFINED;
}

// emitVolume({ pos, dir, radius, spread, length, density, color, rise, turbulence })
static JSValue j_emitVolume(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    DslState* st = state_of(c);
    if (n < 1 || !JS_IsObject(a[0])) {
        st->set_error("emitVolume: expected an object argument");
        return JS_UNDEFINED;
    }
    JSValueConst obj = a[0];
    dsl::VolumeEmitter e{};

    // pos (required array of 3)
    {
        JSValue v = JS_GetPropertyStr(c, obj, "pos");
        if (JS_IsUndefined(v) || JS_IsNull(v)) {
            JS_FreeValue(c, v);
            st->set_error("emitVolume: pos is required");
            return JS_UNDEFINED;
        }
        for (int i = 0; i < 3; ++i) {
            JSValue ei = JS_GetPropertyUint32(c, v, (uint32_t)i);
            e.pos[i] = (float)argd(c, ei);
            JS_FreeValue(c, ei);
        }
        JS_FreeValue(c, v);
    }

    // dir (optional array of 3, default {0,1,0})
    {
        JSValue v = JS_GetPropertyStr(c, obj, "dir");
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            for (int i = 0; i < 3; ++i) {
                JSValue ei = JS_GetPropertyUint32(c, v, (uint32_t)i);
                e.dir[i] = (float)argd(c, ei);
                JS_FreeValue(c, ei);
            }
        }
        JS_FreeValue(c, v);
    }

    // color (optional array of 3, default {1,1,1})
    {
        JSValue v = JS_GetPropertyStr(c, obj, "color");
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            for (int i = 0; i < 3; ++i) {
                JSValue ei = JS_GetPropertyUint32(c, v, (uint32_t)i);
                e.color[i] = (float)argd(c, ei);
                JS_FreeValue(c, ei);
            }
        }
        JS_FreeValue(c, v);
    }

    // Scalar fields (all optional with defaults from the struct initializer)
    double d;
    if (opt_num(c, obj, "radius",     d)) e.radius     = (float)d;
    if (opt_num(c, obj, "spread",     d)) e.spread     = (float)d;
    if (opt_num(c, obj, "length",     d)) e.length     = (float)d;
    if (opt_num(c, obj, "density",    d)) e.density    = (float)d;
    if (opt_num(c, obj, "rise",       d)) e.rise       = (float)d;
    if (opt_num(c, obj, "turbulence", d)) e.turbulence = (float)d;

    st->emit_volume(e);
    return JS_UNDEFINED;
}

void install_bindings(JSContext* ctx) {
    JSValue g = JS_GetGlobalObject(ctx);
    auto bind=[&](const char* n, JSCFunction* f, int argc){ JS_SetPropertyStr(ctx,g,n,JS_NewCFunction(ctx,f,n,argc)); };
    bind("__dsl_pushMatrix",j_pushMatrix,0); bind("__dsl_popMatrix",j_popMatrix,0);
    bind("__dsl_translate",j_translate,3);
    bind("__dsl_rotateX",j_rotateX,1); bind("__dsl_rotateY",j_rotateY,1); bind("__dsl_rotateZ",j_rotateZ,1);
    bind("__dsl_scale",j_scale,3); bind("__dsl_applyMatrix",j_applyMatrix,1);
    bind("__dsl_lookAt",j_lookAt,6);
    bind("__dsl_fill",j_fill,1); bind("__dsl_tint",j_tint,4);
    bind("__dsl_beginVoxels",j_beginVoxels,1); bind("__dsl_endVoxels",j_endVoxels,0);
    bind("__dsl_beginRig",j_beginRig,1); bind("__dsl_root",j_root,3); bind("__dsl_bone",j_bone,3);
    bind("__dsl_rigPush",j_rigPush,0); bind("__dsl_rigPop",j_rigPop,0); bind("__dsl_atJoint",j_atJoint,1);
    bind("__dsl_radius",j_radius,1); bind("__dsl_socket",j_socket,2); bind("__dsl_mirrorBranch",j_mirrorBranch,3); bind("__dsl_endRig",j_endRig,0); bind("__dsl_skin",j_skin,2); bind("__dsl_segments",j_segments,2); bind("__dsl_attach",j_attach,4); bind("__dsl_bind",j_bind_geometry,2);
    bind("__dsl_beginClip",j_beginClip,3); bind("__dsl_clipDuration",j_clipDuration,1); bind("__dsl_clipRate",j_clipRate,1); bind("__dsl_clipLoop",j_clipLoop,1); bind("__dsl_clipMode",j_clipMode,1); bind("__dsl_clipAt",j_clipAt,1); bind("__dsl_clipMarker",j_clipMarker,2); bind("__dsl_clipKey",j_clipKey,3); bind("__dsl_generate",j_generate,1); bind("__dsl_endClip",j_endClip,0);
    bind("__dsl_beginMotion",j_beginMotion,1); bind("__dsl_input",j_motionInput,2); bind("__dsl_target",j_motionTarget,2); bind("__dsl_controller",j_motionController,3); bind("__dsl_clipNode",j_clipNode,2); bind("__dsl_blend1D",j_blendNode,3); bind("__dsl_additive",j_additiveNode,3); bind("__dsl_nativeController",j_nativeNode,2); bind("__dsl_output",j_outputNode,2); bind("__dsl_endMotion",j_endMotion,0);
    bind("__dsl_sphere",j_sphere,4); bind("__dsl_box",j_box,6);
    bind("__dsl_op",j_op,1); bind("__dsl_smoothing",j_smoothing,1);
    bind("__dsl_raycast",j_raycast,6);
    bind("__dsl_beginModifier",j_beginModifier,0); bind("__dsl_endModifier",j_endModifier,1);
    bind("__dsl_placeChild",j_placeChild,2);
    bind("__dsl_beginShape",j_beginShape,1); bind("__dsl_vertex",j_vertex,3);
    bind("__dsl_endShape",j_endShape,0); bind("__dsl_line",j_line,8);
    bind("__dsl_capsule",j_capsule,7); bind("__dsl_cylinder",j_cylinder,7);
    bind("__dsl_cone",j_cone,8);
    bind("__dsl_beginContour",j_beginContour,0); bind("__dsl_endContour",j_endContour,0);
    bind("__dsl_joinType",j_joinType,1); bind("__dsl_extrude",j_extrude,1);
    bind("__dsl_position",j_position,0);
    // Volumetric emitter binding.
    bind("__dsl_emitVolume",j_emitVolume,1);
    // Terrain verb binding (Task 5: terrainVolume).
    bind("__terrainVolume",j_terrainVolume,4);
    // World query verbs (Task 7: heightAt/slopeAt/moistureAt/biomeAt).
    bind("__heightAt",j_heightAt,2);
    bind("__slopeAt",j_slopeAt,2);
    bind("__moistureAt",j_moistureAt,2);
    bind("__biomeAt",j_biomeAt,2);
    // Tileset verb bindings.
    bind("__dsl_ts_tile",j_ts_tile,5); bind("__dsl_ts_base",j_ts_base,2);
    bind("__dsl_ts_layer",j_ts_layer,2); bind("__dsl_ts_dropChild",j_ts_dropChild,2);
    bind("__dsl_ts_variant",j_ts_variant,1);
    // Params-fn `r` helper natives (draw from ts->param_rng during layer()).
    bind("__dsl_ts_rng_int",j_ts_rng_int,1); bind("__dsl_ts_rng_float",j_ts_rng_float,2);
    // Override Math.random with the seeded draw so authored parts are reproducible.
    JSValue math = JS_GetPropertyStr(ctx, g, "Math");
    if (JS_IsObject(math)) {
        JS_SetPropertyStr(ctx, math, "random", JS_NewCFunction(ctx, j_random, "random", 0));
    }
    JS_FreeValue(ctx, math);
    install_pf_bindings(ctx);
    JS_FreeValue(ctx,g);
}

} // namespace dsl
