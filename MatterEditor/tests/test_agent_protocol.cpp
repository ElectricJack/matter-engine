#include "../src/agent_protocol.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

using matter::jsondoc::Value;
using viewer::agent::Protocol;
using viewer::agent::Status;

#define CHECK(condition, message)                                                \
    do {                                                                         \
        if (!(condition)) {                                                       \
            std::fprintf(stderr, "FAIL: %s (%s:%d)\n", message, __FILE__,       \
                         __LINE__);                                               \
            std::exit(1);                                                        \
        }                                                                        \
    } while (false)

Value string_value(std::string text) {
    Value value;
    value.kind = Value::Kind::String;
    value.str = std::move(text);
    return value;
}

Value object_value() {
    Value value;
    value.kind = Value::Kind::Object;
    return value;
}

struct Fixture {
    viewer::agent::Context context{true, 7, 9, 11, 13, 15, 17, 19};
    std::vector<std::string> lines;
    Protocol protocol{
        true,
        [this](const std::string& line, std::string&) {
            lines.push_back(line);
            return true;
        },
        [this]() { return context; }};

    Fixture() {
        CHECK(protocol.add_command({"agent.commands", "List commands", {},
                                    "object", false, {}}),
              "register commands descriptor");
        CHECK(protocol.add_command(
                  {"agent.help", "Help",
                   {{"command", "string", true, "target"}}, "object", false,
                   {}}),
              "register help descriptor");
        CHECK(protocol.add_command(
                  {"future.command", "Future", {}, "object", false,
                   [](const viewer::agent::Context&) {
                       return viewer::agent::Availability{
                           false, Status::UnsupportedCommand, "not built"};
                   }}),
              "register unavailable descriptor");
        CHECK(protocol.add_command({"scene.ready", "Needs scene", {}, "object",
                                    true, {}}),
              "register readiness descriptor");
    }
};

Value parse_line(const std::string& line) {
    Value value;
    std::string error;
    CHECK(viewer::agent::parse_json_strict(line, value, error),
          error.empty() ? "parse output" : error.c_str());
    return value;
}

std::string field_string(const Value& value, const char* key) {
    const Value* field = value.find(key);
    CHECK(field && field->kind == Value::Kind::String, "expected string field");
    return field->str;
}

void test_strict_json_and_framing() {
    Value value;
    std::string error;
    CHECK(viewer::agent::parse_json_strict(
              R"({"text":"quote: \" slash: \\ snowman: \u2603"})", value,
              error),
          "strict parser accepts escapes and unicode");
    CHECK(value.find("text") && value.find("text")->str.find("\xe2\x98\x83") !=
                                      std::string::npos,
          "unicode escape decoded as UTF-8");
    for (const std::string& malformed : {
             std::string("{"), std::string("{} trailing"),
             std::string(R"({"x":01})"), std::string(R"({"x":"\q"})"),
             std::string(R"({"x":1,"x":2})")}) {
        error.clear();
        CHECK(!viewer::agent::parse_json_strict(malformed, value, error) &&
                  !error.empty(),
              "strict parser rejects malformed input");
    }
    std::string invalid_utf8 = "{\"x\":\"";
    invalid_utf8.push_back(static_cast<char>(0xc0));
    invalid_utf8 += "\"}";
    error.clear();
    CHECK(!viewer::agent::parse_json_strict(invalid_utf8, value, error),
          "strict parser rejects invalid UTF-8");
    error.clear();
    CHECK(!viewer::agent::parse_json_strict(
              std::string(viewer::agent::kMaxRequestBytes + 1, 'x'), value,
              error),
          "request byte limit enforced");

    viewer::agent::LineBuffer framing;
    viewer::agent::LineBuffer::Line line;
    const std::string first = "agent {\"ver";
    framing.append(first.data(), first.size());
    CHECK(!framing.pop(line), "partial input is retained");
    const std::string remainder = "sion\":1}\r\nnext\n";
    framing.append(remainder.data(), remainder.size());
    CHECK(framing.pop(line) && line.text == "agent {\"version\":1}",
          "partial CRLF request is reconstructed");
    CHECK(framing.pop(line) && line.text == "next", "second line framed");
    framing.append("old partial", 11);
    framing.reset();
    framing.append("new\n", 4);
    CHECK(framing.pop(line) && line.text == "new",
          "reconnect reset drops old partial bytes");
    const std::string oversized(viewer::agent::kMaxRequestBytes + 2, 'a');
    framing.append(oversized.data(), oversized.size());
    framing.append("\nok\n", 4);
    CHECK(framing.pop(line) && line.oversized,
          "oversized line becomes explicit framing error");
    CHECK(framing.pop(line) && line.text == "ok", "framer recovers at newline");
}

void test_identity_and_discovery() {
    const std::uint64_t maximum = UINT64_MAX;
    const Value encoded = viewer::agent::object_identity_json(
        {viewer::agent::ObjectIdentity::Kind::BakedRoot, maximum});
    CHECK(field_string(encoded, "kind") == "baked_root" &&
              field_string(encoded, "id") == "18446744073709551615",
          "object identity is typed and lossless");
    viewer::agent::ObjectIdentity decoded;
    std::string error;
    CHECK(viewer::agent::parse_object_identity(encoded, decoded, error) &&
              decoded.id == maximum &&
              decoded.kind == viewer::agent::ObjectIdentity::Kind::BakedRoot,
          "object identity round trips");
    Value numeric = object_value();
    Value id;
    id.kind = Value::Kind::UInt64;
    id.uint64_value = maximum;
    numeric.set("kind", string_value("entity"));
    numeric.set("id", id);
    CHECK(!viewer::agent::parse_object_identity(numeric, decoded, error),
          "numeric object ids are rejected");

    Fixture fixture;
    const Value discovery = fixture.protocol.discovery();
    const Value* commands = discovery.find("commands");
    CHECK(commands && commands->kind == Value::Kind::Array &&
              commands->arr.size() == 4,
          "discovery lists all descriptors");
    CHECK(field_string(commands->arr[0], "name") == "agent.commands" &&
              field_string(commands->arr[2], "name") == "future.command",
          "discovery ordering is deterministic");
    const Value help = fixture.protocol.help("agent.help");
    CHECK(help.find("arguments") && help.find("arguments")->arr.size() == 1,
          "help exposes arguments");
    const Value schema = fixture.protocol.schema("agent.help");
    CHECK(schema.find("limits") && schema.find("object_identity") &&
              schema.find("command") && schema.find("request") &&
              schema.find("request")->kind == Value::Kind::Object &&
              schema.find("result") &&
              schema.find("result")->kind == Value::Kind::Object,
          "schema exposes envelopes, identity and bounds");
}

void test_request_ticket_and_terminal_result() {
    Fixture fixture;
    const auto start = Protocol::Clock::now();
    const auto begun = fixture.protocol.begin(
        R"({"version":1,"request_id":"req-\"one","command":"agent.commands","args":{},"timeout_ms":1000})",
        start);
    CHECK(begun.accepted && begun.request.request_id == "req-\"one",
          "escaped request id accepted");
    CHECK(fixture.protocol.attach_ticket(begun.request.request_id,
                                         18446744073709551615ull),
          "registry ticket attached");
    Value payload = object_value();
    payload.set("answer", string_value("line\nwith\tcontrols"));
    CHECK(fixture.protocol.complete(begun.request.request_id,
                                    18446744073709551615ull, Status::Ok,
                                    payload),
          "terminal result accepted once");
    CHECK(!fixture.protocol.complete(begun.request.request_id,
                                     18446744073709551615ull, Status::Ok,
                                     payload),
          "second terminal result suppressed");
    CHECK(fixture.lines.size() == 1, "exactly one terminal result emitted");
    const Value result = parse_line(fixture.lines[0]);
    CHECK(field_string(result, "request_id") == "req-\"one" &&
              field_string(result, "ticket_id") == "18446744073709551615" &&
              field_string(result, "code") == "ok",
          "request and ticket ids correlate losslessly");
    const Value* context = result.find("context");
    CHECK(context && context->find("session") && context->find("scene") &&
              context->find("selection") && context->find("frame") &&
              context->find("view"),
          "terminal result carries every revision identity");
}

void test_explicit_failures_and_duplicates() {
    Fixture fixture;
    fixture.protocol.begin(
        R"({"version":1,"request_id":"unknown","command":"no.such","args":{}})");
    CHECK(field_string(parse_line(fixture.lines.back()), "code") ==
              "unknown_command",
          "unknown command fails explicitly");
    fixture.protocol.begin(
        R"({"version":1,"request_id":"unsupported","command":"future.command","args":{}})");
    CHECK(field_string(parse_line(fixture.lines.back()), "code") ==
              "unsupported_command",
          "unsupported command fails explicitly");

    fixture.context.scene_ready = false;
    fixture.protocol.begin(
        R"({"version":1,"request_id":"not-ready","command":"scene.ready","args":{}})");
    CHECK(field_string(parse_line(fixture.lines.back()), "code") == "not_ready",
          "not-ready status is distinct");
    fixture.context.scene_ready = true;

    const auto accepted = fixture.protocol.begin(
        R"({"version":1,"request_id":"missing","command":"agent.help","args":{"command":"absent"}})");
    CHECK(accepted.accepted && fixture.protocol.reject_accepted(
                                  "missing", Status::NotFound, "not registered"),
          "not-found request rejected after routing validation");
    CHECK(field_string(parse_line(fixture.lines.back()), "code") == "not_found",
          "not-found status is distinct");

    fixture.protocol.begin(
        R"({"version":1,"request_id":"dup","command":"agent.commands","args":{}})");
    const std::size_t before_duplicate = fixture.lines.size();
    fixture.protocol.begin(
        R"({"version":1,"request_id":"dup","command":"agent.commands","args":{}})");
    CHECK(fixture.lines.size() == before_duplicate + 1 &&
              field_string(parse_line(fixture.lines.back()), "code") ==
                  "duplicate_request_id",
          "duplicate request id fails explicitly without replacing pending request");

    fixture.protocol.begin(
        R"({"version":1,"request_id":"stale","command":"agent.commands","args":{},"expect":{"scene_revision":"12"}})");
    CHECK(field_string(parse_line(fixture.lines.back()), "code") ==
              "stale_revision",
          "stale expected revision is distinct");
    fixture.protocol.begin("{\"version\":1", Protocol::Clock::now());
    const Value malformed = parse_line(fixture.lines.back());
    CHECK(field_string(malformed, "code") == "invalid_input" &&
              malformed.find("request_id") &&
              malformed.find("request_id")->kind == Value::Kind::Null,
          "malformed input gets request-less invalid terminal result");
}

void test_timeout_delayed_completion_and_output_bound() {
    Fixture fixture;
    const auto start = Protocol::Clock::now();
    const auto begun = fixture.protocol.begin(
        R"({"version":1,"request_id":"slow","command":"agent.commands","args":{},"timeout_ms":10})",
        start);
    CHECK(begun.accepted && fixture.protocol.attach_ticket("slow", 23),
          "delayed request attached");
    fixture.protocol.expire(start + std::chrono::milliseconds(11));
    CHECK(fixture.lines.size() == 1 &&
              field_string(parse_line(fixture.lines[0]), "code") == "timeout",
          "deadline produces timeout terminal");
    CHECK(!fixture.protocol.complete("slow", 23, Status::Ok, object_value()) &&
              fixture.lines.size() == 1,
          "late ticket completion cannot emit a second terminal");

    const auto large = fixture.protocol.begin(
        R"({"version":1,"request_id":"large","command":"agent.commands","args":{}})");
    CHECK(large.accepted && fixture.protocol.attach_ticket("large", 24),
          "large request attached");
    Value payload = object_value();
    payload.set("blob", string_value(
                            std::string(viewer::agent::kMaxResultBytes, 'x')));
    CHECK(fixture.protocol.complete("large", 24, Status::Ok, std::move(payload)),
          "oversized result replaced with bounded terminal");
    CHECK(fixture.lines.back().size() < viewer::agent::kMaxResultBytes &&
              fixture.lines.back().find("\"code\":\"output_too_large\"") !=
                  std::string::npos,
          "output limit reports output_too_large");

    const auto failed = fixture.protocol.begin(
        R"({"version":1,"request_id":"failed","command":"agent.commands","args":{}})");
    CHECK(failed.accepted && fixture.protocol.attach_ticket("failed", 25) &&
              fixture.protocol.complete("failed", 25,
                                        Status::ExecutionFailure, object_value(),
                                        "handler rejected command"),
          "execution failure completes its ticket");
    CHECK(field_string(parse_line(fixture.lines.back()), "code") ==
              "execution_failure",
          "execution failure remains distinct");
}

}  // namespace

int main() {
    test_strict_json_and_framing();
    test_identity_and_discovery();
    test_request_ticket_and_terminal_result();
    test_explicit_failures_and_duplicates();
    test_timeout_delayed_completion_and_output_bound();
    std::printf("agent protocol tests: ALL PASS\n");
    return 0;
}
