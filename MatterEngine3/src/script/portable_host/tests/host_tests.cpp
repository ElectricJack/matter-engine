// Public header consumer: no QuickJS, editor, graphics or bake includes.
#include <matter/portable_host/quickjs_host.h>
#include <chrono>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <thread>

namespace d = matter::contracts;
namespace s = matter::services;
namespace h = matter::portable_host;
using E = d::ErrorCode;
const s::JobEnvelope envelope{"host-test", "revision-1", 7};
const std::string valid_source = R"JS(import { RecipeBuilder } from 'matter:authoring';
export default context => {
  const b = new RecipeBuilder(context);
  b.output(b.box('box', [1, 2, 3]));
  return b.build();
};)JS";
void check(bool pass, const std::string &message) { if (!pass) throw std::runtime_error(message); }
s::SourceModule module(std::string id, std::string source, std::vector<d::SemanticId> dependencies = {}) {
  return {{{std::move(id)}, "fixture://source.js", "1", {}, {}}, std::move(source), std::move(dependencies)};
}
s::SourceSnapshot snapshot(std::string source = valid_source) {
  return {{"fixture/main.mjs"}, d::sha256(source), {module("fixture/main.mjs", std::move(source))}, {}};
}
std::string errors(const s::RecipeResult &result) {
  std::string out;
  for (const auto &d : result.diagnostics()) {
    out += std::string(d::code_name(d.code)) + ": " + d.message;
    if (d.source) out += " at " + d.source->uri + ":" + std::to_string(d.source->line) + ":" + std::to_string(d.source->column);
    out += '\n';
  }
  return out;
}
void failed(const s::RecipeResult &result, E code) {
  check(!result.ok() && !result.value(), "failure published recipe");
  check(result.envelope() == envelope, "lost job envelope");
  for (const auto &d : result.diagnostics()) if (d.code == code) return;
  throw std::runtime_error("expected " + std::string(d::code_name(code)) + ", got " + errors(result));
}
void passed(const s::RecipeResult &result) { check(result.ok(), errors(result)); }
int controls() {
  auto host = h::make_quickjs_host();
  const auto author = [&](const s::SourceSnapshot &ss, s::ResourceBudget b = {}, s::JobControl c = {}) {
    return host->author(envelope, ss, {}, b, c);
  };
  auto owned = author(snapshot()); passed(owned);
  check(owned.value()->nodes.size() == 1 && owned.envelope() == envelope, "owned recipe/envelope mismatch");
  check(h::quickjs_sdk_content().hex.size() == 64, "SDK identity missing");
  for (int i = 0; i < 8; ++i) { passed(author(snapshot())); failed(author(snapshot("throw Error('again');")), E::backend_failure); }
  check(owned.value()->nodes[0].id.value == "box", "recipe lost after runtime destruction");
  std::cout << "PASS HOST-OWNERSHIP-v1\n";

  auto imports = snapshot("import size from './helper.mjs'; " + valid_source);
  imports.modules[0].dependencies = {{"fixture/helper.mjs"}};
  imports.modules.push_back(module("fixture/helper.mjs", "export default 2;"));
  passed(author(imports));
  imports.modules[0].dependencies.clear(); failed(author(imports), E::missing_dependency);
  imports.modules.pop_back(); failed(author(imports), E::missing_dependency);
  failed(author(snapshot("import x from 'node:fs'; export default () => x;")), E::missing_dependency);
  failed(author(snapshot("import x from '../../escape.mjs';")), E::missing_dependency);
  failed(author(snapshot("import x from 'matter-internal/bridge.mjs';")), E::missing_dependency);
  auto cycle = snapshot(); cycle.modules[0].dependencies = {{"fixture/main.mjs"}}; failed(author(cycle), E::cycle);
  auto duplicate = snapshot(); duplicate.modules.push_back(duplicate.modules[0]); failed(author(duplicate), E::invalid_input);
  auto forged = snapshot(); forged.modules[0].identity.content = d::sha256("wrong"); failed(author(forged), E::invalid_input);
  forged = snapshot(); forged.modules[0].source_utf8 += '\0'; failed(author(forged), E::invalid_input);
  forged = snapshot(); forged.modules[0].source_utf8 += static_cast<char>(0xff); failed(author(forged), E::invalid_input);
  forged = snapshot(); forged.modules[0].identity.id.value = "fixture/../main.mjs"; failed(author(forged), E::invalid_input);
  std::cout << "PASS HOST-SNAPSHOT-IMPORTS-v1\n";

  auto syntax = author(snapshot("export default function( {\n")); failed(syntax, E::backend_failure);
  check(syntax.diagnostics()[0].source && syntax.diagnostics()[0].source->uri == "fixture://source.js" && syntax.diagnostics()[0].source->line > 0, "syntax source missing");
  auto runtime = author(snapshot("export default () => {\n  throw new Error('source marker');\n};"));
  failed(runtime, E::backend_failure);
  check(runtime.diagnostics()[0].source && runtime.diagnostics()[0].source->line == 2, "runtime source line missing");
  auto sdk = author(snapshot(R"JS(import {RecipeBuilder} from 'matter:authoring';
export default ctx => { const b = new RecipeBuilder(ctx);
 return b.at({uri:'author://prop',line:17,column:4}, () => { b.output(b.box('bad', [-1,1,1])); return b.build(); }); };)JS"));
  failed(sdk, E::invalid_input);
  check(sdk.diagnostics()[0].source && sdk.diagnostics()[0].source->uri == "author://prop" && sdk.diagnostics()[0].source->line == 17, "SDK source diagnostic lost");
  auto unsupported = host->author(envelope, snapshot(), {{d::Value::Object{}}, 0, {{"unknown.required", 1}}}, {}, {});
  failed(unsupported, E::unsupported_capability);
  auto unsupported_source = valid_source;
  unsupported_source.replace(unsupported_source.find("return b.build();"), std::string("return b.build();").size(),
    "return {...b.build(),required_capabilities:[{name:'unknown.required',version:1}]};");
  failed(author(snapshot(unsupported_source)), E::unsupported_capability);
  failed(author(snapshot("export default () => ({bad: NaN});")), E::invalid_input);
  failed(author(snapshot("export default () => ({get bad() {throw Error('getter ran');}});")), E::invalid_input);
  failed(author(snapshot("export default () => {let a={}; a.self=a; return a;};")), E::cycle);
  std::cout << "PASS HOST-SOURCE-DIAGNOSTICS-v1\n";

  auto async = snapshot("await Promise.resolve();\n" + valid_source);
  passed(author(async));
  async = snapshot("import {RecipeBuilder} from 'matter:authoring'; export default async ctx => {const b=new RecipeBuilder(ctx); b.output(b.box('box',[1,1,1])); const hash=await b.digest(); if(hash.hex.length!==64) throw Error('digest'); return b.build();};");
  passed(author(async));
  async = snapshot("export default async () => { const m = await import('./helper.mjs'); return m.default; }; ");
  async.modules[0].dependencies = {{"fixture/helper.mjs"}};
  async.modules.push_back(module("fixture/helper.mjs", "throw Error('dynamic source');"));
  failed(author(async), E::backend_failure);
  failed(author(snapshot("export default () => new Promise(() => {});")), E::missing_dependency);
  failed(author(snapshot("Promise.reject(new Error('unhandled'));\n" + valid_source)), E::backend_failure);
  std::cout << "PASS HOST-ASYNC-v1\n";

  s::CancellationSource cancelled; cancelled.cancel();
  failed(author(snapshot(), {}, {cancelled.token(), {}}), E::cancelled);
  s::ResourceBudget short_time; short_time.execution_microseconds = 100000;
  failed(author(snapshot("for (;;) {}"), short_time), E::resource_limit);
  failed(author(snapshot("export default () => { for (;;) {} };"), short_time), E::resource_limit);
  failed(author(snapshot("export default () => new Promise(() => {function spin(){Promise.resolve().then(spin);} spin();});"), short_time), E::resource_limit);
  std::atomic<bool> started{false}; s::CancellationSource active;
  std::thread interrupter([&] { while (!started.load()) std::this_thread::yield(); std::this_thread::sleep_for(std::chrono::milliseconds(10)); active.cancel(); });
  s::JobControl control{active.token(), [&](const s::Progress &) { started.store(true); }};
  auto interrupted = author(snapshot("export default () => { for (;;) {} };"), {}, control);
  started.store(true); interrupter.join(); failed(interrupted, E::cancelled);
  s::CancellationSource late;
  failed(author(snapshot(), {}, {late.token(), [&](const s::Progress &p) { if (p.fraction == 1) late.cancel(); }}), E::cancelled);
  failed(author(snapshot(), {}, {{}, [](const s::Progress &) { throw std::runtime_error("callback"); }}), E::backend_failure);
  std::cout << "PASS HOST-INTERRUPT-DEADLINE-v1\n";

  s::ResourceBudget b; b.working_bytes = 1024; failed(author(snapshot(), b), E::resource_limit);
  b = {}; b.working_bytes = 2 * 1024 * 1024;
  failed(author(snapshot("export default () => { let a=[]; for (;;) a.push(new Array(1024).fill(0)); };"), b), E::resource_limit);
  // Exercise repeated OOM while constructing exceptions and backtraces, at
  // differing allocation boundaries (the pinned VM needed an ownership fix).
  for (uint64_t bytes = 512 * 1024; bytes <= 2 * 1024 * 1024; bytes += 131071) {
    b.working_bytes = bytes;
    failed(author(snapshot("export default () => { let a=[]; for (;;) a.push(new Error('heap pressure')); };"), b), E::resource_limit);
  }
  std::cout << "PASS HOST-OOM-BACKTRACE-OWNERSHIP-v1\n";
  b = {}; b.output_bytes = 2048;
  auto oversized = valid_source;
  oversized.replace(oversized.find("return b.build();"), std::string("return b.build();").size(),
                    "return {...b.build(),metadata:{padding:'x'.repeat(4096)}};");
  failed(author(snapshot(oversized), b), E::resource_limit);
  b = {}; b.operations = 2; failed(author(snapshot(), b), E::resource_limit);
  auto deep = snapshot(); deep.modules[0].dependencies = {{"fixture/helper.mjs"}};
  deep.modules.push_back(module("fixture/helper.mjs", "export default 1;", {{"fixture/child.mjs"}}));
  deep.modules.push_back(module("fixture/child.mjs", "export default 1;"));
  b = {}; b.dependency_depth = 2; failed(author(deep, b), E::resource_limit);
  failed(author(snapshot("export default function recurse(){return recurse()+1;}")), E::resource_limit);
  std::cout << "PASS HOST-RESOURCE-LIMITS-v1\n";
  auto isolated = snapshot("if (globalThis.probe) throw Error('shared VM'); globalThis.probe=true;\n" + valid_source);
  passed(author(isolated)); passed(author(isolated));
  passed(author(snapshot("for (const name of ['std','os','require','process','document','fetch','Date','Atomics','SharedArrayBuffer']) if (typeof globalThis[name] !== 'undefined') throw Error(name);\n" + valid_source)));
  failed(author(snapshot("Math.random();\n" + valid_source)), E::backend_failure);
  std::cout << "PASS HOST-NO-AMBIENT-SERVICES-v1\n";
  passed(author(snapshot(R"JS(
for (const width of [0,1,32,63,64,65,128]) {
  const modulus = 1n << BigInt(width);
  for (const n of [-1n, 0n, 1n, 0xffffffffffffffffn, -0xffffffffffffffffn]) {
    const expected = ((n % modulus) + modulus) % modulus;
    if (BigInt.asUintN(width,n) !== expected) throw Error('unsigned BigInt conversion');
  }
}
if ((0xffffffffffffffffn).toString(16) !== 'ffffffffffffffff') throw Error('BigInt radix formatting');
let calls=0; BigInt.asUintN({valueOf(){++calls;return 64;}},-1n);
if(calls!==1) throw Error('width converted twice');
let rejected=false; try { BigInt.asUintN(64n,1n); } catch { rejected=true; }
if(!rejected) throw Error('BigInt width accepted');
)JS" + valid_source)));
  std::cout << "PASS HOST-BIGINT-CONFORMANCE-v1\n";
  // The vendor intrinsics themselves (no setup.js override): limb-boundary
  // widths, signed truncation, every binary radix plus 10/36 against a
  // division reference, and the allocation ceiling for huge widths.
  passed(author(snapshot(R"JS(
const values = [0n, 1n, -1n, 2n, -2n, 0x7fffffffn, 0x80000000n, -0x80000000n, 0xffffffffn,
  2n**63n - 1n, 2n**63n, -(2n**63n), 2n**64n - 1n, -(2n**64n - 1n), 2n**64n, 2n**95n,
  2n**96n - 1n, 2n**126n, 2n**127n + 12345n, 2n**128n - 1n, -(2n**128n), 0x9e3779b97f4a7c15n];
for (const width of [0,1,2,31,32,33,63,64,65,95,96,97,127,128,129,200]) {
  const modulus = 1n << BigInt(width), half = modulus >> 1n;
  for (const n of values) {
    const u = ((n % modulus) + modulus) % modulus, s = width && u >= half ? u - modulus : u;
    if (BigInt.asUintN(width, n) !== u) throw Error(`asUintN(${width}, ${n})`);
    if (BigInt.asIntN(width, n) !== s) throw Error(`asIntN(${width}, ${n})`);
  }
}
const digits = '0123456789abcdefghijklmnopqrstuvwxyz';
for (const radix of [2, 4, 8, 10, 16, 32, 36]) {
  for (const n of values) {
    let a = n < 0n ? -n : n, expected = '';
    do { expected = digits[Number(a % BigInt(radix))] + expected; a /= BigInt(radix); } while (a);
    if (n.toString(radix) !== (n < 0n ? '-' : '') + expected) throw Error(`${n}.toString(${radix})`);
  }
}
if ((2n**63n).toString(2) !== '1' + '0'.repeat(63)) throw Error('binary radix formatting');
let range = false; try { BigInt.asUintN(2**53 - 1, -1n); } catch (e) { range = e instanceof RangeError; }
if (!range) throw Error('oversized asUintN accepted');
if (BigInt.asUintN(2**53 - 1, 5n) !== 5n || BigInt.asIntN(2**53 - 1, -5n) !== -5n) throw Error('wide identity');
)JS" + valid_source)));
  std::cout << "PASS HOST-BIGINT-VENDOR-v1\n";
  return 0;
}
const d::Value::Object &object(const d::Value &v) { return std::get<d::Value::Object>(v.data); }
const std::string &string(const d::Value &v) { return std::get<std::string>(v.data); }
std::string parity_request(const d::Value &decoded) {
  std::string input;
  check(d::canonical_json(decoded, input).ok(), "invalid fixture input");
  const auto &root = object(decoded);
  s::SourceSnapshot ss; ss.entry.value = string(root.at("entry")); ss.content = d::sha256(input);
  for (const auto &v : std::get<d::Value::Array>(root.at("modules").data)) {
    const auto &o = object(v); auto m = module(string(o.at("id")), string(o.at("source")));
    m.identity.uri = string(o.at("uri"));
    for (const auto &dep : std::get<d::Value::Array>(o.at("dependencies").data)) m.dependencies.push_back({string(dep)});
    ss.modules.push_back(std::move(m));
  }
  h::QuickJsOptions options;
  if (root.count("legacy_voxels")) options.validation.supported_capabilities.push_back({"matter.legacy.voxel-meshing", 1});
  s::AuthoringRequest request; request.seed = std::stoull(string(root.at("seed")), nullptr, 16); request.parameters = root.at("parameters");
  auto result = h::make_quickjs_host(options)->author(envelope, ss, request, {}, {});
  check(result.ok(), errors(result));
  std::string out; check(d::encode(*result.value(), out, options.validation).ok(), "encode failed");
  return out;
}
int parity(bool batch) {
  std::string input{std::istreambuf_iterator<char>(std::cin), {}};
  d::Value decoded; check(d::parse_json(input, decoded).ok(), "invalid fixture input");
  if (batch) {
    d::Value::Array results;
    for (const auto &request : std::get<d::Value::Array>(decoded.data))
      results.emplace_back(parity_request(request));
    std::string output;
    check(d::canonical_json(d::Value(std::move(results)), output).ok(), "batch encoding failed");
    std::cout << output;
  } else {
    std::cout << parity_request(decoded);
  }
  return 0;
}
int main(int argc, char **argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "--stdin") return parity(false);
    if (argc == 2 && std::string(argv[1]) == "--batch") return parity(true);
    return controls();
  }
  catch (const std::exception &e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
