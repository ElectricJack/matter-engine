// Native side of the parity/lifetime harness. Reads one JSON step per stdin
// line, drives the production C ABI (bindings/geometry-c) and the production
// admission rule (matter::services::admit), and prints one JSON outcome line
// per step. It never decides pass/fail; run_parity.py compares the outcomes
// with the WASM run and the frozen baseline.
#include <matter/contracts/jobs.h>
#include <matter/geometry_c.h>

#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {
namespace data = matter::contracts;
namespace services = matter::services;
#define HEADER(T) matter_abi_header{sizeof(T), MATTER_ABI_MAJOR, MATTER_ABI_MINOR}

struct Held {
  std::string instance;
  matter_result_handle handle = 0;
  services::JobEnvelope job;
};
struct Control {
  bool cancel_now = false, cancel_on_progress = false;
  unsigned progress = 0;
};
uint32_t cancelled(void *user) { return static_cast<Control *>(user)->cancel_now ? 1u : 0u; }
void progressed(void *user, double, matter_bytes_view) {
  auto *control = static_cast<Control *>(user);
  ++control->progress;
  if (control->cancel_on_progress) control->cancel_now = true;
}

[[noreturn]] void fatal(const std::string &message) {
  std::fprintf(stderr, "native_runner: %s\n", message.c_str());
  std::exit(2);
}
const data::Value::Object &object(const data::Value &value, const char *what) {
  auto *o = std::get_if<data::Value::Object>(&value.data);
  if (!o) fatal(std::string(what) + " must be an object");
  return *o;
}
const data::Value *field(const data::Value::Object &o, const char *key) {
  auto it = o.find(key);
  return it == o.end() ? nullptr : &it->second;
}
std::string text(const data::Value::Object &o, const char *key, const char *fallback = nullptr) {
  const data::Value *v = field(o, key);
  if (!v) {
    if (fallback) return fallback;
    fatal(std::string("missing string field ") + key);
  }
  auto *s = std::get_if<std::string>(&v->data);
  if (!s) fatal(std::string(key) + " must be a string");
  return *s;
}
// Integers arrive as JSON numbers (exact below 2^53) or decimal strings.
uint64_t integer(const data::Value &v, const char *key) {
  if (auto *d = std::get_if<double>(&v.data)) {
    if (*d < 0 || *d > 9007199254740992.0 || *d != static_cast<double>(static_cast<uint64_t>(*d)))
      fatal(std::string(key) + " must be an exact non-negative integer");
    return static_cast<uint64_t>(*d);
  }
  if (auto *s = std::get_if<std::string>(&v.data)) {
    if (s->empty() || s->size() > 20) fatal(std::string(key) + " is not a decimal integer");
    uint64_t n = 0;
    for (char c : *s) {
      if (c < '0' || c > '9') fatal(std::string(key) + " is not a decimal integer");
      n = n * 10 + static_cast<uint64_t>(c - '0');
    }
    return n;
  }
  fatal(std::string(key) + " must be an integer");
}
uint64_t hex64(const std::string &s) {
  if (s.size() != 16) fatal("generation_epoch must be 16 lowercase hex digits");
  uint64_t n = 0;
  for (char c : s) {
    int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    if (d < 0) fatal("generation_epoch must be 16 lowercase hex digits");
    n = (n << 4) | static_cast<uint64_t>(d);
  }
  return n;
}
services::JobEnvelope job_of(const data::Value::Object &step, const char *key) {
  const data::Value *v = field(step, key);
  if (!v) fatal(std::string("missing job ") + key);
  const auto &o = object(*v, key);
  return {text(o, "request_id"), text(o, "source_revision"), hex64(text(o, "generation_epoch"))};
}
std::string quoted(const std::string &s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') out += '\\', out += static_cast<char>(c);
    else if (c < 0x20) {
      char buffer[8];
      std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
      out += buffer;
    } else out += static_cast<char>(c);
  }
  return out + "\"";
}
std::string handle_text(matter_result_handle h) {
  char buffer[24];
  std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(h));
  return buffer;
}

class Runner {
public:
  ~Runner() {
    for (auto &[name, api] : instances_)
      if (owners_[name] && api.instance) matter_geometry_dispose(&api);
  }
  std::string run(const data::Value::Object &step) {
    const std::string op = text(step, "op");
    if (op == "create") return create(step);
    if (op == "copy_table") {
      instances_[text(step, "as")] = table(step);
      owners_[text(step, "as")] = false;
      return outcome(MATTER_OK);
    }
    if (op == "dispose") return outcome(matter_geometry_dispose(&table(step)));
    if (op == "capabilities") return info(step, false);
    if (op == "evaluate" || op == "query" || op == "process") return call(op, step);
    if (op == "read") return read(step);
    if (op == "release") {
      Held &held = result(step);
      return outcome(table(step).release(table(step).instance, held.handle));
    }
    if (op == "admit") return admit(step);
    if (op == "repeat") return repeat(step);
    fatal("unknown op " + op);
  }

private:
  matter_geometry_api &table(const data::Value::Object &step) {
    auto it = instances_.find(text(step, "instance"));
    if (it == instances_.end()) fatal("unknown instance " + text(step, "instance"));
    return it->second;
  }
  Held &result(const data::Value::Object &step, const char *key = "result") {
    auto it = results_.find(text(step, key));
    if (it == results_.end()) fatal("unknown result " + text(step, key));
    return it->second;
  }
  static std::string outcome(matter_status status, const std::string &extra = "") {
    return "{\"status\":" + std::to_string(status) + extra + "}";
  }
  std::string diagnostics(matter_geometry_api &api) {
    uint64_t required = 0;
    if (api.diagnostics(api.instance, {nullptr, 0}, &required) != MATTER_RESOURCE_LIMIT) return "null";
    std::vector<uint8_t> buffer(static_cast<size_t>(required));
    if (api.diagnostics(api.instance, {buffer.data(), buffer.size()}, &required) != MATTER_OK) return "null";
    return std::string(buffer.begin(), buffer.end());
  }
  std::string create(const data::Value::Object &step) {
    matter_geometry_config config{HEADER(matter_geometry_config), 0, 0, 0, 0, 0};
    if (matter_geometry_default_config(&config) != MATTER_OK) fatal("default config failed");
    if (const data::Value *limits = field(step, "limits")) {
      for (const auto &[key, value] : object(*limits, "limits")) {
        if (key == "max_results") config.max_results = static_cast<uint32_t>(integer(value, "max_results"));
        else if (key == "max_retained_bytes") config.max_retained_bytes = integer(value, "max_retained_bytes");
        else if (key == "max_request_bytes") config.max_request_bytes = integer(value, "max_request_bytes");
        else fatal("unknown limit " + key);
      }
    }
    matter_geometry_api api{};
    api.header = HEADER(matter_geometry_api);
    matter_status status = matter_geometry_create(&config, &api);
    if (status == MATTER_OK) {
      instances_[text(step, "instance")] = api;
      owners_[text(step, "instance")] = true;
    }
    return outcome(status);
  }
  std::string info(const data::Value::Object &step, bool diagnostic) {
    matter_geometry_api &api = table(step);
    auto fn = diagnostic ? api.diagnostics : api.capabilities;
    uint64_t required = 0;
    matter_status first = fn(api.instance, {nullptr, 0}, &required);
    if (first != MATTER_RESOURCE_LIMIT) return outcome(first);
    std::vector<uint8_t> buffer(static_cast<size_t>(required));
    matter_status status = fn(api.instance, {buffer.data(), buffer.size()}, &required);
    if (status != MATTER_OK) return outcome(status);
    return outcome(status, ",\"wire\":" + std::string(buffer.begin(), buffer.end()));
  }
  std::string call(const std::string &op, const data::Value::Object &step) {
    matter_geometry_api &api = table(step);
    const std::string request = text(step, "request");
    services::JobEnvelope envelope = job_of(step, "job");
    matter_job_envelope job{};
    job.header = HEADER(matter_job_envelope);
    job.request_id_utf8 = {reinterpret_cast<const uint8_t *>(envelope.request_id.data()), envelope.request_id.size()};
    job.source_revision_utf8 = {reinterpret_cast<const uint8_t *>(envelope.source_revision.data()), envelope.source_revision.size()};
    job.generation_epoch = envelope.generation_epoch;
    matter_budget budget{};
    budget.header = HEADER(matter_budget);
    if (matter_geometry_default_budget(&budget) != MATTER_OK) fatal("default budget failed");
    if (const data::Value *limits = field(step, "budget")) {
      for (const auto &[key, value] : object(*limits, "budget")) {
        uint64_t n = integer(value, key.c_str());
        if (key == "operations") budget.operations = n;
        else if (key == "vertices") budget.vertices = n;
        else if (key == "indices") budget.indices = n;
        else if (key == "output_bytes") budget.output_bytes = n;
        else if (key == "working_bytes") budget.working_bytes = n;
        else if (key == "execution_microseconds") budget.execution_microseconds = n;
        else if (key == "queries") budget.queries = n;
        else if (key == "dependency_depth") budget.dependency_depth = static_cast<uint32_t>(n);
        else if (key == "progress_events") budget.progress_events = static_cast<uint32_t>(n);
        else fatal("unknown budget " + key);
      }
    }
    const std::string cancel = text(step, "cancel", "none");
    if (cancel != "none" && cancel != "pre" && cancel != "progress") fatal("unknown cancel mode " + cancel);
    Control state{cancel == "pre", cancel == "progress", 0};
    matter_control control{HEADER(matter_control), &state, cancelled, progressed};
    const matter_control *controlled = cancel == "none" ? nullptr : &control;
    matter_bytes_view bytes{reinterpret_cast<const uint8_t *>(request.data()), request.size()};
    matter_call_result r{};
    if (op == "evaluate") r = api.evaluate(api.instance, bytes, &job, &budget, controlled);
    else {
      Held &source = result(step, "source");
      r = (op == "query" ? api.query : api.process)(api.instance, source.handle, bytes, &job, &budget, controlled);
    }
    std::string extra = ",\"progress_events\":" + std::to_string(state.progress);
    if (r.status != MATTER_OK) {
      if (r.result != 0) fatal("failed call returned a handle");
      return outcome(r.status, extra + ",\"diagnostics\":" + diagnostics(api));
    }
    results_[text(step, "as")] = {text(step, "instance"), r.result, envelope};
    if (handles_) handles_->insert(r.result);
    extra += ",\"handle\":" + quoted(handle_text(r.result));
    if (text(step, "emit", "wire") == "wire") {
      matter_bytes_view view{};
      matter_status status = api.read(api.instance, r.result, &view);
      if (status != MATTER_OK) fatal("read of a fresh result failed");
      extra += ",\"wire_bytes\":" + std::to_string(view.byte_length) + ",\"wire\":" +
               std::string(reinterpret_cast<const char *>(view.data), static_cast<size_t>(view.byte_length));
    }
    return outcome(MATTER_OK, extra);
  }
  std::string read(const data::Value::Object &step) {
    matter_geometry_api &api = table(step);
    Held &held = result(step);
    matter_bytes_view view{};
    matter_status status = api.read(api.instance, held.handle, &view);
    if (status != MATTER_OK) {
      if (view.data != nullptr || view.byte_length != 0) fatal("failed read produced a view");
      return outcome(status);
    }
    return outcome(status, ",\"wire_bytes\":" + std::to_string(view.byte_length) + ",\"wire\":" +
                               std::string(reinterpret_cast<const char *>(view.data), static_cast<size_t>(view.byte_length)));
  }
  // Coordinator-side publication: a result whose envelope no longer matches
  // the expected one is stale and must leave the previously admitted owner.
  std::string admit(const data::Value::Object &step) {
    Held &held = result(step);
    matter_geometry_api &api = instances_.at(held.instance);
    matter_bytes_view view{};
    if (api.read(api.instance, held.handle, &view) != MATTER_OK) fatal("admit requires a live result");
    auto job_result = services::JobResult<std::string>::success(
        held.job, std::string(reinterpret_cast<const char *>(view.data), static_cast<size_t>(view.byte_length)));
    data::ValidationResult v = services::admit(job_result, job_of(step, "expected"), published_);
    std::string codes = "[";
    for (size_t i = 0; i < v.diagnostics.size(); ++i)
      codes += (i ? "," : "") + quoted(data::code_name(v.diagnostics[i].code));
    codes += "]";
    if (v.ok()) published_name_ = text(step, "result");
    return "{\"status\":" + std::string(v.ok() ? "0" : quoted(data::code_name(v.diagnostics.at(0).code))) +
           ",\"codes\":" + codes + ",\"published\":" + (published_ ? quoted(published_name_) : "null") +
           ",\"published_bytes\":" + std::to_string(published_ ? published_->size() : 0) + "}";
  }

  // Repeated allocation/disposal: runs the inner steps count times and
  // reports how often each op ended in each status, plus how many distinct
  // handles the successful calls produced (handles are never reused).
  std::string repeat(const data::Value::Object &step) {
    if (handles_) fatal("nested repeat");
    const data::Value *repeats = field(step, "count");
    if (!repeats) fatal("repeat requires count");
    const uint64_t count = integer(*repeats, "count");
    const data::Value *inner = field(step, "steps");
    auto *steps = inner ? std::get_if<data::Value::Array>(&inner->data) : nullptr;
    if (!steps || steps->empty()) fatal("repeat requires steps");
    std::set<matter_result_handle> handles;
    std::map<std::string, uint64_t> outcomes;
    handles_ = &handles;
    for (uint64_t i = 0; i < count; ++i) {
      for (const auto &value : *steps) {
        const auto &o = object(value, "repeat step");
        const std::string out = run(o);
        // Every outcome line starts {"status":<number or quoted code>.
        const size_t begin = std::strlen("{\"status\":");
        const size_t end = out.find_first_of(",}", begin);
        std::string status = out.substr(begin, end - begin);
        if (!status.empty() && status.front() == '"') status = status.substr(1, status.size() - 2);
        ++outcomes[text(o, "op") + ":" + status];
      }
    }
    handles_ = nullptr;
    std::string counts = "{";
    for (const auto &[key, n] : outcomes) counts += (counts.size() > 1 ? "," : "") + quoted(key) + ":" + std::to_string(n);
    return "{\"status\":0,\"iterations\":" + std::to_string(count) + ",\"outcomes\":" + counts +
           "},\"distinct_handles\":" + std::to_string(handles.size()) + "}";
  }

  std::map<std::string, matter_geometry_api> instances_;
  std::map<std::string, bool> owners_;
  std::map<std::string, Held> results_;
  std::shared_ptr<const std::string> published_;
  std::string published_name_;
  std::set<matter_result_handle> *handles_ = nullptr;
};
} // namespace

int main() {
  data::ValidationLimits limits;
  limits.max_string_bytes = 16u * 1024u * 1024u;
  Runner runner;
  std::string line;
  unsigned index = 0;
  while (std::getline(std::cin, line)) {
    if (line.empty()) continue;
    data::Value step;
    data::ValidationResult parsed = data::parse_json(line, step, limits);
    if (!parsed.ok()) fatal("step " + std::to_string(index) + " is not strict JSON");
    std::string out = runner.run(object(step, "step"));
    std::cout << out << '\n' << std::flush;
    ++index;
  }
  return 0;
}
