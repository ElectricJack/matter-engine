#pragma once

// Versioned JSONL protocol layered on MATTER_CMD_FIFO.  The FIFO remains the
// byte transport and CommandRegistry remains the command dispatcher; this file
// only owns framing, validation, discovery metadata, request/ticket correlation
// and terminal result serialization.

#include "matter/json_doc.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace viewer::agent {

constexpr std::uint32_t kProtocolVersion = 1;
constexpr std::size_t kMaxRequestBytes = 64u * 1024u;
constexpr std::size_t kMaxResultBytes = 1024u * 1024u;
constexpr std::uint32_t kDefaultTimeoutMs = 5000;
constexpr std::uint32_t kMaxTimeoutMs = 30000;
constexpr std::size_t kMaxPendingRequests = 256;
constexpr std::size_t kRememberedRequestIds = 4096;

enum class Status {
    Ok,
    InvalidInput,
    NotReady,
    NotFound,
    StaleRevision,
    Timeout,
    ExecutionFailure,
    UnknownCommand,
    UnsupportedCommand,
    DuplicateRequestId,
    OutputTooLarge,
};

const char* to_string(Status status);

// Value constructors for the hand-built result payloads.  Trivial, but there
// were three copies of them in main.cpp alone before this existed, and a
// four-line `Kind::Number` literal repeated per call site is exactly the noise
// that hides the one that sets the wrong kind.
namespace json {

inline matter::jsondoc::Value object() {
    matter::jsondoc::Value value;
    value.kind = matter::jsondoc::Value::Kind::Object;
    return value;
}

inline matter::jsondoc::Value array() {
    matter::jsondoc::Value value;
    value.kind = matter::jsondoc::Value::Kind::Array;
    return value;
}

inline matter::jsondoc::Value number(double input) {
    matter::jsondoc::Value value;
    value.kind = matter::jsondoc::Value::Kind::Number;
    value.num = input;
    return value;
}

inline matter::jsondoc::Value boolean(bool input) {
    matter::jsondoc::Value value;
    value.kind = matter::jsondoc::Value::Kind::Bool;
    value.b = input;
    return value;
}

inline matter::jsondoc::Value string(std::string input) {
    matter::jsondoc::Value value;
    value.kind = matter::jsondoc::Value::Kind::String;
    value.str = std::move(input);
    return value;
}

// Counters and ids cross this protocol as decimal STRINGS, never as JSON
// numbers: an IEEE-754 double silently truncates a 64-bit id.
inline matter::jsondoc::Value decimal(std::uint64_t input) {
    return string(std::to_string(input));
}

inline matter::jsondoc::Value vec3(float x, float y, float z) {
    matter::jsondoc::Value value = array();
    for (float component : {x, y, z}) value.arr.push_back(number(component));
    return value;
}

// The repeated "we do not have this, and here is why" shape.  Availability is
// never faked anywhere in this protocol, so the negative case has a spelling.
inline matter::jsondoc::Value unavailable(std::string reason) {
    matter::jsondoc::Value value = object();
    value.set("available", boolean(false));
    value.set("reason", string(std::move(reason)));
    return value;
}

}  // namespace json

// Object ids are never emitted as JSON numbers.  The kind is part of identity:
// entity "42" and baked_root "42" are distinct objects.
struct ObjectIdentity {
    enum class Kind { Entity, BakedRoot } kind = Kind::Entity;
    std::uint64_t id = 0;
};

matter::jsondoc::Value object_identity_json(const ObjectIdentity& object);
bool parse_object_identity(const matter::jsondoc::Value& value,
                           ObjectIdentity& object, std::string& error);

// Every terminal result carries this identity snapshot.  Counters serialize as
// decimal strings to remain lossless in JavaScript and other IEEE-754 clients.
struct Context {
    bool scene_ready = false;
    std::uint64_t session_id = 0;
    std::uint64_t session_generation = 0;
    std::uint64_t scene_generation = 0;
    std::uint64_t scene_revision = 0;
    std::uint64_t selection_revision = 0;
    std::uint64_t frame_id = 0;
    std::uint64_t view_id = 0;
};

matter::jsondoc::Value context_json(const Context& context);

struct Availability {
    bool available = true;
    Status unavailable_status = Status::UnsupportedCommand;
    std::string reason;
};

struct Argument {
    std::string name;
    // string, number, integer, boolean, object, array, or object_id
    std::string type;
    bool required = false;
    std::string description;
};

struct CommandDescriptor {
    std::string name;
    std::string summary;
    std::vector<Argument> arguments;
    std::string result_type = "object";
    bool requires_scene_ready = false;
    std::function<Availability(const Context&)> availability;
};

struct ExpectedContext {
    std::optional<std::uint64_t> session_generation;
    std::optional<std::uint64_t> scene_generation;
    std::optional<std::uint64_t> scene_revision;
    std::optional<std::uint64_t> selection_revision;
    std::optional<std::uint64_t> frame_id;
    std::optional<std::uint64_t> view_id;
};

struct Request {
    std::string request_id;
    std::string command;
    matter::jsondoc::Value arguments;
    ExpectedContext expected;
    std::uint32_t timeout_ms = kDefaultTimeoutMs;
};

struct BeginResult {
    bool accepted = false;
    Request request;
};

// Strict, bounded JSON used only by the hostile-input protocol boundary.  It
// deliberately does not change jsondoc::parse_json's legacy lenient contract.
bool parse_json_strict(const std::string& text, matter::jsondoc::Value& out,
                       std::string& error);
std::string write_json_strict(const matter::jsondoc::Value& value);

// Incremental newline framing shared by POSIX reads and Windows file polling.
// reset() is used when a polled Windows command file is truncated/replaced so
// an old partial request can never concatenate with bytes from the new file.
class LineBuffer {
public:
    struct Line {
        std::string text;
        bool oversized = false;
    };

    void append(const char* bytes, std::size_t size);
    bool pop(Line& line);
    void reset();
    std::size_t buffered_bytes() const { return partial_.size(); }

private:
    std::string partial_;
    std::deque<Line> lines_;
    bool discarding_oversized_ = false;
};

using OutputSink =
    std::function<bool(const std::string& json_line, std::string& error)>;

// Append one complete JSONL record with reader-sharing semantics on Windows.
OutputSink jsonl_file_sink(std::string path);

class Protocol {
public:
    using Clock = std::chrono::steady_clock;
    using ContextProvider = std::function<Context()>;

    Protocol(bool output_available, OutputSink sink,
             ContextProvider context_provider);

    bool add_command(CommandDescriptor descriptor);
    const CommandDescriptor* find_command(const std::string& name) const;

    matter::jsondoc::Value discovery() const;
    matter::jsondoc::Value help(const std::string& name) const;
    matter::jsondoc::Value schema(const std::string& name) const;

    // Parses, validates and reserves a request id.  Rejections emit their one
    // terminal response immediately.  Accepted requests must next be attached
    // to the CommandRegistry ticket returned by dispatch().
    BeginResult begin(const std::string& json,
                      Clock::time_point now = Clock::now());
    bool attach_ticket(const std::string& request_id, std::uint64_t ticket_id);
    bool reject_accepted(const std::string& request_id, Status status,
                         const std::string& message);
    bool complete(const std::string& request_id, std::uint64_t ticket_id,
                  Status status, matter::jsondoc::Value result,
                  const std::string& message = {});
    void expire(Clock::time_point now = Clock::now());

    std::size_t pending_count() const { return pending_.size(); }
    std::string take_io_error();

private:
    struct Pending {
        std::string command;
        std::uint64_t ticket_id = 0;
        Clock::time_point deadline{};
    };

    Context current_context() const;
    Availability command_availability(const CommandDescriptor& descriptor,
                                      const Context& context) const;
    bool emit_terminal(const std::string* request_id,
                       const std::string* command,
                       std::optional<std::uint64_t> ticket_id, Status status,
                       matter::jsondoc::Value result,
                       const std::string& message);
    void remember_request_id(const std::string& request_id);

    bool output_available_ = false;
    OutputSink sink_;
    ContextProvider context_provider_;
    std::vector<CommandDescriptor> commands_;
    std::unordered_map<std::string, Pending> pending_;
    std::unordered_set<std::string> seen_ids_;
    std::deque<std::string> seen_order_;
    std::string io_error_;
};

}  // namespace viewer::agent
