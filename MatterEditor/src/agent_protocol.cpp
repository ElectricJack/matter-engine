#include "agent_protocol.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <set>
#include <sstream>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace viewer::agent {
namespace {

using Value = matter::jsondoc::Value;

Value null_value() { return Value{}; }
Value bool_value(bool value) {
    Value out;
    out.kind = Value::Kind::Bool;
    out.b = value;
    return out;
}
Value number_value(double value) {
    Value out;
    out.kind = Value::Kind::Number;
    out.num = value;
    return out;
}
Value string_value(std::string value) {
    Value out;
    out.kind = Value::Kind::String;
    out.str = std::move(value);
    return out;
}
Value object_value() {
    Value out;
    out.kind = Value::Kind::Object;
    return out;
}
Value array_value() {
    Value out;
    out.kind = Value::Kind::Array;
    return out;
}

bool has_control(const std::string& value) {
    return std::any_of(value.begin(), value.end(), [](unsigned char c) {
        return c < 0x20 || c == 0x7f;
    });
}

bool decimal_u64(const std::string& text, std::uint64_t& value) {
    if (text.empty() ||
        !std::all_of(text.begin(), text.end(), [](char c) {
            return c >= '0' && c <= '9';
        }))
        return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
    if (errno == ERANGE || !end || *end != '\0') return false;
    value = static_cast<std::uint64_t>(parsed);
    return true;
}

bool integral_u32(const Value& value, std::uint32_t& out) {
    if (value.kind == Value::Kind::UInt64) {
        if (value.uint64_value > std::numeric_limits<std::uint32_t>::max())
            return false;
        out = static_cast<std::uint32_t>(value.uint64_value);
        return true;
    }
    if (value.kind != Value::Kind::Number || !std::isfinite(value.num) ||
        value.num < 0.0 || std::floor(value.num) != value.num ||
        value.num > std::numeric_limits<std::uint32_t>::max())
        return false;
    out = static_cast<std::uint32_t>(value.num);
    return true;
}

const Value* unique_field(const Value& object, const std::string& key) {
    const Value* result = nullptr;
    for (const auto& entry : object.obj) {
        if (entry.first != key) continue;
        if (result) return nullptr;
        result = &entry.second;
    }
    return result;
}

bool valid_utf8(const std::string& text) {
    std::size_t index = 0;
    const auto continuation = [&](std::size_t offset) {
        return index + offset < text.size() &&
               (static_cast<unsigned char>(text[index + offset]) & 0xc0u) ==
                   0x80u;
    };
    while (index < text.size()) {
        const unsigned char first = static_cast<unsigned char>(text[index]);
        if (first <= 0x7fu) {
            ++index;
        } else if (first >= 0xc2u && first <= 0xdfu && continuation(1)) {
            index += 2;
        } else if (first >= 0xe0u && first <= 0xefu && continuation(1) &&
                   continuation(2)) {
            const unsigned char second =
                static_cast<unsigned char>(text[index + 1]);
            if ((first == 0xe0u && second < 0xa0u) ||
                (first == 0xedu && second >= 0xa0u))
                return false;
            index += 3;
        } else if (first >= 0xf0u && first <= 0xf4u && continuation(1) &&
                   continuation(2) && continuation(3)) {
            const unsigned char second =
                static_cast<unsigned char>(text[index + 1]);
            if ((first == 0xf0u && second < 0x90u) ||
                (first == 0xf4u && second >= 0x90u))
                return false;
            index += 4;
        } else {
            return false;
        }
    }
    return true;
}

class StrictParser {
public:
    StrictParser(const std::string& text, std::string& error)
        : text_(text), error_(error) {}

    bool parse(Value& out) {
        if (text_.size() > kMaxRequestBytes)
            return fail("request exceeds 65536 byte limit");
        skip_space();
        if (!value(out, 0)) return false;
        skip_space();
        if (position_ != text_.size()) return fail("trailing data after JSON value");
        return true;
    }

private:
    static constexpr std::size_t kMaxDepth = 32;
    static constexpr std::size_t kMaxNodes = 4096;
    static constexpr std::size_t kMaxStringBytes = 16384;

    bool fail(const char* message) {
        if (error_.empty()) {
            error_ = std::string(message) + " at byte " +
                     std::to_string(position_);
        }
        return false;
    }

    void skip_space() {
        while (position_ < text_.size() &&
               (text_[position_] == ' ' || text_[position_] == '\t' ||
                text_[position_] == '\r' || text_[position_] == '\n'))
            ++position_;
    }

    bool take(char expected) {
        if (position_ >= text_.size() || text_[position_] != expected)
            return false;
        ++position_;
        return true;
    }

    bool value(Value& out, std::size_t depth) {
        if (depth > kMaxDepth) return fail("JSON nesting depth exceeds 32");
        if (++nodes_ > kMaxNodes) return fail("JSON value count exceeds 4096");
        skip_space();
        if (position_ >= text_.size()) return fail("incomplete JSON value");
        switch (text_[position_]) {
            case '{': return object(out, depth + 1);
            case '[': return array(out, depth + 1);
            case '"':
                out.kind = Value::Kind::String;
                return string(out.str);
            case 't': return literal("true", Value::Kind::Bool, out, true);
            case 'f': return literal("false", Value::Kind::Bool, out, false);
            case 'n': return literal("null", Value::Kind::Null, out, false);
            default: return number(out);
        }
    }

    bool literal(const char* token, Value::Kind kind, Value& out, bool b) {
        const std::size_t length = std::strlen(token);
        if (text_.compare(position_, length, token) != 0)
            return fail("invalid JSON literal");
        position_ += length;
        out.kind = kind;
        out.b = b;
        return true;
    }

    static bool hex_digit(char c, unsigned& value) {
        if (c >= '0' && c <= '9') value = static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f') value = 10u + static_cast<unsigned>(c - 'a');
        else if (c >= 'A' && c <= 'F') value = 10u + static_cast<unsigned>(c - 'A');
        else return false;
        return true;
    }

    bool unicode_escape(std::uint32_t& codepoint) {
        if (position_ + 4 > text_.size()) return fail("incomplete unicode escape");
        codepoint = 0;
        for (int index = 0; index < 4; ++index) {
            unsigned digit = 0;
            if (!hex_digit(text_[position_++], digit))
                return fail("invalid unicode escape");
            codepoint = (codepoint << 4u) | digit;
        }
        return true;
    }

    bool append_codepoint(std::string& out, std::uint32_t cp) {
        if (cp <= 0x7f) out.push_back(static_cast<char>(cp));
        else if (cp <= 0x7ff) {
            out.push_back(static_cast<char>(0xc0u | (cp >> 6u)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3fu)));
        } else if (cp <= 0xffff) {
            out.push_back(static_cast<char>(0xe0u | (cp >> 12u)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 6u) & 0x3fu)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3fu)));
        } else if (cp <= 0x10ffff) {
            out.push_back(static_cast<char>(0xf0u | (cp >> 18u)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 12u) & 0x3fu)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 6u) & 0x3fu)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3fu)));
        } else {
            return fail("unicode codepoint out of range");
        }
        return out.size() <= kMaxStringBytes || fail("JSON string exceeds 16384 bytes");
    }

    bool string(std::string& out) {
        if (!take('"')) return fail("expected JSON string");
        out.clear();
        while (position_ < text_.size()) {
            const unsigned char raw = static_cast<unsigned char>(text_[position_++]);
            if (raw == '"')
                return valid_utf8(out) || fail("invalid UTF-8 in string");
            if (raw < 0x20) return fail("unescaped control character in string");
            if (raw != '\\') {
                out.push_back(static_cast<char>(raw));
                if (out.size() > kMaxStringBytes)
                    return fail("JSON string exceeds 16384 bytes");
                continue;
            }
            if (position_ >= text_.size()) return fail("incomplete string escape");
            const char escaped = text_[position_++];
            switch (escaped) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    std::uint32_t cp = 0;
                    if (!unicode_escape(cp)) return false;
                    if (cp >= 0xd800 && cp <= 0xdbff) {
                        if (position_ + 2 > text_.size() || text_[position_] != '\\' ||
                            text_[position_ + 1] != 'u')
                            return fail("high surrogate without low surrogate");
                        position_ += 2;
                        std::uint32_t low = 0;
                        if (!unicode_escape(low)) return false;
                        if (low < 0xdc00 || low > 0xdfff)
                            return fail("invalid low surrogate");
                        cp = 0x10000u + ((cp - 0xd800u) << 10u) + (low - 0xdc00u);
                    } else if (cp >= 0xdc00 && cp <= 0xdfff) {
                        return fail("unpaired low surrogate");
                    }
                    if (!append_codepoint(out, cp)) return false;
                    break;
                }
                default: return fail("invalid string escape");
            }
            if (out.size() > kMaxStringBytes)
                return fail("JSON string exceeds 16384 bytes");
        }
        return fail("unterminated JSON string");
    }

    bool object(Value& out, std::size_t depth) {
        take('{');
        out.kind = Value::Kind::Object;
        out.obj.clear();
        skip_space();
        if (take('}')) return true;
        std::set<std::string> keys;
        while (true) {
            skip_space();
            std::string key;
            if (!string(key)) return false;
            if (!keys.insert(key).second) return fail("duplicate object key");
            skip_space();
            if (!take(':')) return fail("expected ':' after object key");
            Value child;
            if (!value(child, depth)) return false;
            out.obj.emplace_back(std::move(key), std::move(child));
            skip_space();
            if (take('}')) return true;
            if (!take(',')) return fail("expected ',' or '}' in object");
        }
    }

    bool array(Value& out, std::size_t depth) {
        take('[');
        out.kind = Value::Kind::Array;
        out.arr.clear();
        skip_space();
        if (take(']')) return true;
        while (true) {
            Value child;
            if (!value(child, depth)) return false;
            out.arr.push_back(std::move(child));
            skip_space();
            if (take(']')) return true;
            if (!take(',')) return fail("expected ',' or ']' in array");
        }
    }

    bool number(Value& out) {
        const std::size_t start = position_;
        if (take('-') && position_ >= text_.size()) return fail("incomplete number");
        if (take('0')) {
            if (position_ < text_.size() && text_[position_] >= '0' &&
                text_[position_] <= '9')
                return fail("leading zero in number");
        } else {
            if (position_ >= text_.size() || text_[position_] < '1' ||
                text_[position_] > '9')
                return fail("invalid number");
            while (position_ < text_.size() && text_[position_] >= '0' &&
                   text_[position_] <= '9')
                ++position_;
        }
        if (take('.')) {
            if (position_ >= text_.size() || text_[position_] < '0' ||
                text_[position_] > '9')
                return fail("fraction requires a digit");
            while (position_ < text_.size() && text_[position_] >= '0' &&
                   text_[position_] <= '9')
                ++position_;
        }
        if (position_ < text_.size() &&
            (text_[position_] == 'e' || text_[position_] == 'E')) {
            ++position_;
            if (position_ < text_.size() &&
                (text_[position_] == '+' || text_[position_] == '-'))
                ++position_;
            if (position_ >= text_.size() || text_[position_] < '0' ||
                text_[position_] > '9')
                return fail("exponent requires a digit");
            while (position_ < text_.size() && text_[position_] >= '0' &&
                   text_[position_] <= '9')
                ++position_;
        }
        const std::string token = text_.substr(start, position_ - start);
        const bool unsigned_integer = token.find_first_not_of("0123456789") ==
                                      std::string::npos;
        if (unsigned_integer) {
            std::uint64_t exact = 0;
            if (!decimal_u64(token, exact)) return fail("integer out of range");
            constexpr std::uint64_t kMaxExactDouble = 9007199254740992ull;
            if (exact > kMaxExactDouble) {
                out.kind = Value::Kind::UInt64;
                out.uint64_value = exact;
                return true;
            }
        }
        errno = 0;
        char* end = nullptr;
        const double parsed = std::strtod(token.c_str(), &end);
        if (errno == ERANGE || !end || *end != '\0' || !std::isfinite(parsed))
            return fail("number out of range");
        out.kind = Value::Kind::Number;
        out.num = parsed;
        return true;
    }

    const std::string& text_;
    std::string& error_;
    std::size_t position_ = 0;
    std::size_t nodes_ = 0;
};

void write_escaped(const std::string& value, std::string& out) {
    static constexpr char kHex[] = "0123456789abcdef";
    out.push_back('"');
    for (unsigned char c : value) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out.push_back(kHex[c >> 4u]);
                    out.push_back(kHex[c & 0x0fu]);
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
}

void write_value(const Value& value, std::string& out) {
    switch (value.kind) {
        case Value::Kind::Null: out += "null"; break;
        case Value::Kind::Bool: out += value.b ? "true" : "false"; break;
        case Value::Kind::Number: {
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "%.17g", value.num);
            out += buffer;
            break;
        }
        case Value::Kind::UInt64: out += std::to_string(value.uint64_value); break;
        case Value::Kind::String: write_escaped(value.str, out); break;
        case Value::Kind::Array:
            out.push_back('[');
            for (std::size_t index = 0; index < value.arr.size(); ++index) {
                if (index) out.push_back(',');
                write_value(value.arr[index], out);
            }
            out.push_back(']');
            break;
        case Value::Kind::Object:
            out.push_back('{');
            for (std::size_t index = 0; index < value.obj.size(); ++index) {
                if (index) out.push_back(',');
                write_escaped(value.obj[index].first, out);
                out.push_back(':');
                write_value(value.obj[index].second, out);
            }
            out.push_back('}');
            break;
    }
}

bool type_matches(const Value& value, const std::string& type,
                  std::string& error) {
    if (type == "string") return value.kind == Value::Kind::String;
    if (type == "number")
        return value.kind == Value::Kind::Number || value.kind == Value::Kind::UInt64;
    if (type == "integer") {
        return value.kind == Value::Kind::UInt64 ||
               (value.kind == Value::Kind::Number && std::isfinite(value.num) &&
                std::floor(value.num) == value.num);
    }
    if (type == "boolean") return value.kind == Value::Kind::Bool;
    if (type == "object") return value.kind == Value::Kind::Object;
    if (type == "array") return value.kind == Value::Kind::Array;
    if (type == "object_id") {
        ObjectIdentity ignored;
        return parse_object_identity(value, ignored, error);
    }
    error = "descriptor has unknown argument type '" + type + "'";
    return false;
}

bool expected_u64(const Value& object, const char* key,
                  std::optional<std::uint64_t>& output, std::string& error) {
    const Value* value = unique_field(object, key);
    if (!value) return true;
    if (value->kind != Value::Kind::String || !decimal_u64(value->str, output.emplace())) {
        error = std::string("expect.") + key + " must be a decimal string";
        output.reset();
        return false;
    }
    return true;
}

bool expectations_match(const ExpectedContext& expected, const Context& current) {
    return (!expected.session_generation ||
            *expected.session_generation == current.session_generation) &&
           (!expected.scene_generation ||
            *expected.scene_generation == current.scene_generation) &&
           (!expected.scene_revision || *expected.scene_revision == current.scene_revision) &&
           (!expected.selection_revision ||
            *expected.selection_revision == current.selection_revision) &&
           (!expected.frame_id || *expected.frame_id == current.frame_id) &&
           (!expected.view_id || *expected.view_id == current.view_id);
}

}  // namespace

const char* to_string(Status status) {
    switch (status) {
        case Status::Ok: return "ok";
        case Status::InvalidInput: return "invalid_input";
        case Status::NotReady: return "not_ready";
        case Status::NotFound: return "not_found";
        case Status::StaleRevision: return "stale_revision";
        case Status::Timeout: return "timeout";
        case Status::ExecutionFailure: return "execution_failure";
        case Status::UnknownCommand: return "unknown_command";
        case Status::UnsupportedCommand: return "unsupported_command";
        case Status::DuplicateRequestId: return "duplicate_request_id";
        case Status::OutputTooLarge: return "output_too_large";
    }
    return "execution_failure";
}

Value object_identity_json(const ObjectIdentity& object) {
    Value out = object_value();
    out.set("kind", string_value(object.kind == ObjectIdentity::Kind::Entity
                                     ? "entity"
                                     : "baked_root"));
    out.set("id", string_value(std::to_string(object.id)));
    return out;
}

bool parse_object_identity(const Value& value, ObjectIdentity& object,
                           std::string& error) {
    if (value.kind != Value::Kind::Object) {
        error = "object identity must be an object";
        return false;
    }
    const Value* kind = unique_field(value, "kind");
    const Value* id = unique_field(value, "id");
    if (!kind || kind->kind != Value::Kind::String || !id ||
        id->kind != Value::Kind::String) {
        error = "object identity requires string kind and id";
        return false;
    }
    if (kind->str == "entity") object.kind = ObjectIdentity::Kind::Entity;
    else if (kind->str == "baked_root") object.kind = ObjectIdentity::Kind::BakedRoot;
    else {
        error = "object identity kind must be entity or baked_root";
        return false;
    }
    if (!decimal_u64(id->str, object.id) || object.id == 0) {
        error = "object identity id must be a non-zero decimal string";
        return false;
    }
    for (const auto& entry : value.obj) {
        if (entry.first != "kind" && entry.first != "id") {
            error = "unknown object identity field '" + entry.first + "'";
            return false;
        }
    }
    return true;
}

Value context_json(const Context& context) {
    Value out = object_value();
    Value session = object_value();
    session.set("id", string_value(std::to_string(context.session_id)));
    session.set("generation",
                string_value(std::to_string(context.session_generation)));
    Value scene = object_value();
    scene.set("generation", string_value(std::to_string(context.scene_generation)));
    scene.set("revision", string_value(std::to_string(context.scene_revision)));
    scene.set("ready", bool_value(context.scene_ready));
    Value selection = object_value();
    selection.set("revision", string_value(std::to_string(context.selection_revision)));
    Value frame = object_value();
    frame.set("id", string_value(std::to_string(context.frame_id)));
    Value view = object_value();
    view.set("id", string_value(std::to_string(context.view_id)));
    out.set("session", std::move(session));
    out.set("scene", std::move(scene));
    out.set("selection", std::move(selection));
    out.set("frame", std::move(frame));
    out.set("view", std::move(view));
    return out;
}

bool parse_json_strict(const std::string& text, Value& out, std::string& error) {
    error.clear();
    StrictParser parser(text, error);
    return parser.parse(out);
}

std::string write_json_strict(const Value& value) {
    std::string out;
    write_value(value, out);
    return out;
}

void LineBuffer::append(const char* bytes, std::size_t size) {
    for (std::size_t index = 0; index < size; ++index) {
        const char c = bytes[index];
        if (discarding_oversized_) {
            if (c == '\n') {
                lines_.push_back(Line{{}, true});
                discarding_oversized_ = false;
            }
            continue;
        }
        if (c == '\n') {
            if (!partial_.empty() && partial_.back() == '\r') partial_.pop_back();
            lines_.push_back(Line{std::move(partial_), false});
            partial_.clear();
            continue;
        }
        partial_.push_back(c);
        if (partial_.size() > kMaxRequestBytes) {
            partial_.clear();
            discarding_oversized_ = true;
        }
    }
}

bool LineBuffer::pop(Line& line) {
    if (lines_.empty()) return false;
    line = std::move(lines_.front());
    lines_.pop_front();
    return true;
}

void LineBuffer::reset() {
    partial_.clear();
    lines_.clear();
    discarding_oversized_ = false;
}

OutputSink jsonl_file_sink(std::string path) {
    return [path = std::move(path)](const std::string& line,
                                    std::string& error) -> bool {
        const std::string record = line + "\n";
#ifdef _WIN32
        HANDLE file = CreateFileA(path.c_str(), FILE_APPEND_DATA,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE |
                                      FILE_SHARE_DELETE,
                                  nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                  nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            error = "CreateFileA failed for result file (" +
                    std::to_string(GetLastError()) + ")";
            return false;
        }
        DWORD written = 0;
        const bool ok = WriteFile(file, record.data(),
                                  static_cast<DWORD>(record.size()), &written,
                                  nullptr) != 0 &&
                        written == record.size() && FlushFileBuffers(file) != 0;
        if (!ok)
            error = "WriteFile failed for result file (" +
                    std::to_string(GetLastError()) + ")";
        CloseHandle(file);
        return ok;
#else
        const int file = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (file < 0) {
            error = "open failed for result file: " +
                    std::string(std::strerror(errno));
            return false;
        }
        std::size_t offset = 0;
        while (offset < record.size()) {
            const ssize_t wrote =
                write(file, record.data() + offset, record.size() - offset);
            if (wrote < 0 && errno == EINTR) continue;
            if (wrote <= 0) break;
            offset += static_cast<std::size_t>(wrote);
        }
        const bool ok = offset == record.size();
        if (!ok) error = "write failed for result file: " + std::string(std::strerror(errno));
        close(file);
        return ok;
#endif
    };
}

Protocol::Protocol(bool output_available, OutputSink sink,
                   ContextProvider context_provider)
    : output_available_(output_available),
      sink_(std::move(sink)),
      context_provider_(std::move(context_provider)) {}

bool Protocol::add_command(CommandDescriptor descriptor) {
    if (descriptor.name.empty() || descriptor.name.size() > 96 ||
        has_control(descriptor.name) || find_command(descriptor.name))
        return false;
    commands_.push_back(std::move(descriptor));
    std::sort(commands_.begin(), commands_.end(),
              [](const CommandDescriptor& left, const CommandDescriptor& right) {
                  return left.name < right.name;
              });
    return true;
}

const CommandDescriptor* Protocol::find_command(const std::string& name) const {
    auto found = std::lower_bound(
        commands_.begin(), commands_.end(), name,
        [](const CommandDescriptor& descriptor, const std::string& wanted) {
            return descriptor.name < wanted;
        });
    return found != commands_.end() && found->name == name ? &*found : nullptr;
}

Context Protocol::current_context() const {
    return context_provider_ ? context_provider_() : Context{};
}

Availability Protocol::command_availability(
    const CommandDescriptor& descriptor, const Context& context) const {
    if (descriptor.requires_scene_ready && !context.scene_ready)
        return {false, Status::NotReady, "scene is not ready"};
    return descriptor.availability ? descriptor.availability(context)
                                   : Availability{};
}

Value Protocol::discovery() const {
    const Context context = current_context();
    Value root = object_value();
    root.set("protocol", string_value("matter-agent"));
    root.set("version", number_value(kProtocolVersion));
    Value commands = array_value();
    for (const CommandDescriptor& descriptor : commands_) {
        const Availability available = command_availability(descriptor, context);
        Value item = object_value();
        item.set("name", string_value(descriptor.name));
        item.set("summary", string_value(descriptor.summary));
        Value availability = object_value();
        availability.set("available", bool_value(available.available));
        availability.set("code", string_value(available.available
                                                    ? "ok"
                                                    : to_string(available.unavailable_status)));
        if (!available.reason.empty())
            availability.set("reason", string_value(available.reason));
        item.set("availability", std::move(availability));
        commands.arr.push_back(std::move(item));
    }
    root.set("commands", std::move(commands));
    const Value schema_document = schema("agent.commands");
    const Value* limits = schema_document.find("limits");
    root.set("limits", limits ? *limits : object_value());
    return root;
}

Value Protocol::help(const std::string& name) const {
    Value root = object_value();
    const CommandDescriptor* descriptor = find_command(name);
    if (!descriptor) return root;
    const Availability available = command_availability(*descriptor, current_context());
    root.set("name", string_value(descriptor->name));
    root.set("summary", string_value(descriptor->summary));
    root.set("available", bool_value(available.available));
    if (!available.reason.empty()) root.set("reason", string_value(available.reason));
    Value availability = object_value();
    availability.set("available", bool_value(available.available));
    availability.set("code", string_value(available.available
                                                ? "ok"
                                                : to_string(available.unavailable_status)));
    if (!available.reason.empty())
        availability.set("reason", string_value(available.reason));
    root.set("availability", std::move(availability));
    Value arguments = array_value();
    for (const Argument& argument : descriptor->arguments) {
        Value item = object_value();
        item.set("name", string_value(argument.name));
        item.set("type", string_value(argument.type));
        item.set("required", bool_value(argument.required));
        item.set("description", string_value(argument.description));
        arguments.arr.push_back(std::move(item));
    }
    root.set("arguments", std::move(arguments));
    root.set("result_type", string_value(descriptor->result_type));
    return root;
}

Value Protocol::schema(const std::string& name) const {
    Value root = object_value();
    auto type_schema = [](const char* type) {
        Value schema = object_value();
        schema.set("type", string_value(type));
        return schema;
    };
    auto string_array = [](std::initializer_list<const char*> strings) {
        Value values = array_value();
        for (const char* string : strings)
            values.arr.push_back(string_value(string));
        return values;
    };

    Value request = object_value();
    request.set("type", string_value("object"));
    request.set("additional_properties", bool_value(false));
    request.set("required", string_array({"version", "request_id", "command"}));
    Value request_properties = object_value();
    Value version = type_schema("integer");
    version.set("const", number_value(kProtocolVersion));
    request_properties.set("version", std::move(version));
    Value request_id = type_schema("string");
    request_id.set("max_bytes", number_value(128));
    request_properties.set("request_id", std::move(request_id));
    Value command = type_schema("string");
    command.set("max_bytes", number_value(96));
    request_properties.set("command", std::move(command));
    request_properties.set("args", type_schema("object"));
    Value expect = type_schema("object");
    expect.set("additional_properties", bool_value(false));
    Value expect_properties = object_value();
    for (const char* field : {"session_generation", "scene_generation",
                              "scene_revision", "selection_revision",
                              "frame_id", "view_id"}) {
        Value revision = type_schema("string");
        revision.set("format", string_value("uint64 decimal string"));
        expect_properties.set(field, std::move(revision));
    }
    expect.set("properties", std::move(expect_properties));
    request_properties.set("expect", std::move(expect));
    Value timeout = type_schema("integer");
    timeout.set("minimum", number_value(1));
    timeout.set("maximum", number_value(kMaxTimeoutMs));
    timeout.set("default", number_value(kDefaultTimeoutMs));
    request_properties.set("timeout_ms", std::move(timeout));
    request.set("properties", std::move(request_properties));
    root.set("request", std::move(request));

    Value result = object_value();
    result.set("type", string_value("object"));
    result.set("required",
               string_array({"protocol", "version", "type", "request_id",
                             "command", "ticket_id", "ok", "code", "message",
                             "context", "result"}));
    Value result_properties = object_value();
    result_properties.set("protocol", type_schema("string"));
    result_properties.set("version", type_schema("integer"));
    result_properties.set("type", type_schema("string"));
    result_properties.set("request_id", type_schema("string|null"));
    result_properties.set("command", type_schema("string|null"));
    Value ticket = type_schema("string|null");
    ticket.set("format", string_value("uint64 decimal string"));
    result_properties.set("ticket_id", std::move(ticket));
    result_properties.set("ok", type_schema("boolean"));
    Value code = type_schema("string");
    code.set("enum",
             string_array({"ok", "invalid_input", "not_ready", "not_found",
                           "stale_revision", "timeout", "execution_failure",
                           "unknown_command", "unsupported_command",
                           "duplicate_request_id", "output_too_large"}));
    result_properties.set("code", std::move(code));
    result_properties.set("message", type_schema("string"));
    result_properties.set("context", type_schema("object"));
    result_properties.set("result", type_schema("any"));
    result.set("properties", std::move(result_properties));
    root.set("result", std::move(result));
    Value identity = object_value();
    identity.set("kind", string_value("entity|baked_root"));
    identity.set("id", string_value("non-zero decimal string"));
    root.set("object_identity", std::move(identity));
    Value limits = object_value();
    limits.set("request_bytes", number_value(kMaxRequestBytes));
    limits.set("result_bytes", number_value(kMaxResultBytes));
    limits.set("default_timeout_ms", number_value(kDefaultTimeoutMs));
    limits.set("max_timeout_ms", number_value(kMaxTimeoutMs));
    limits.set("max_pending", number_value(kMaxPendingRequests));
    root.set("limits", std::move(limits));
    if (const CommandDescriptor* descriptor = find_command(name))
        root.set("command", help(descriptor->name));
    return root;
}

void Protocol::remember_request_id(const std::string& request_id) {
    seen_ids_.insert(request_id);
    seen_order_.push_back(request_id);
    while (seen_order_.size() > kRememberedRequestIds) {
        const std::string oldest = std::move(seen_order_.front());
        seen_order_.pop_front();
        if (pending_.find(oldest) == pending_.end()) seen_ids_.erase(oldest);
    }
}

BeginResult Protocol::begin(const std::string& json, Clock::time_point now) {
    BeginResult answer;
    Value root;
    std::string error;
    if (!parse_json_strict(json, root, error) || root.kind != Value::Kind::Object) {
        if (error.empty()) error = "request must be a JSON object";
        emit_terminal(nullptr, nullptr, std::nullopt, Status::InvalidInput,
                      null_value(), error);
        return answer;
    }

    const Value* request_id = unique_field(root, "request_id");
    const Value* command = unique_field(root, "command");
    const std::string* id_text = request_id && request_id->kind == Value::Kind::String
                                     ? &request_id->str
                                     : nullptr;
    const std::string* command_text = command && command->kind == Value::Kind::String
                                          ? &command->str
                                          : nullptr;
    auto invalid = [&](const std::string& message) {
        emit_terminal(id_text, command_text, std::nullopt, Status::InvalidInput,
                      null_value(), message);
    };

    static const std::set<std::string> kFields = {
        "version", "request_id", "command", "args", "expect", "timeout_ms"};
    for (const auto& entry : root.obj) {
        if (!kFields.count(entry.first)) {
            invalid("unknown request field '" + entry.first + "'");
            return answer;
        }
    }
    const Value* version = unique_field(root, "version");
    std::uint32_t parsed_version = 0;
    if (!version || !integral_u32(*version, parsed_version) ||
        parsed_version != kProtocolVersion) {
        invalid("version must be 1");
        return answer;
    }
    if (!id_text || id_text->empty() || id_text->size() > 128 ||
        has_control(*id_text)) {
        invalid("request_id must be a non-empty control-free string of at most 128 bytes");
        return answer;
    }
    if (!command_text || command_text->empty() || command_text->size() > 96 ||
        has_control(*command_text)) {
        invalid("command must be a non-empty control-free string of at most 96 bytes");
        return answer;
    }
    if (seen_ids_.count(*id_text)) {
        emit_terminal(id_text, command_text, std::nullopt,
                      Status::DuplicateRequestId, null_value(),
                      "request_id was already used in this editor session");
        return answer;
    }
    remember_request_id(*id_text);

    Request request;
    request.request_id = *id_text;
    request.command = *command_text;
    request.arguments = object_value();
    if (const Value* args = unique_field(root, "args")) {
        if (args->kind != Value::Kind::Object) {
            invalid("args must be an object");
            return answer;
        }
        request.arguments = *args;
    }
    if (const Value* timeout = unique_field(root, "timeout_ms")) {
        if (!integral_u32(*timeout, request.timeout_ms) || request.timeout_ms == 0 ||
            request.timeout_ms > kMaxTimeoutMs) {
            invalid("timeout_ms must be an integer from 1 through 30000");
            return answer;
        }
    }
    if (const Value* expected = unique_field(root, "expect")) {
        if (expected->kind != Value::Kind::Object) {
            invalid("expect must be an object");
            return answer;
        }
        static const std::set<std::string> kExpectedFields = {
            "session_generation", "scene_generation", "scene_revision",
            "selection_revision", "frame_id", "view_id"};
        for (const auto& entry : expected->obj) {
            if (!kExpectedFields.count(entry.first)) {
                invalid("unknown expect field '" + entry.first + "'");
                return answer;
            }
        }
        if (!expected_u64(*expected, "session_generation",
                          request.expected.session_generation, error) ||
            !expected_u64(*expected, "scene_generation",
                          request.expected.scene_generation, error) ||
            !expected_u64(*expected, "scene_revision",
                          request.expected.scene_revision, error) ||
            !expected_u64(*expected, "selection_revision",
                          request.expected.selection_revision, error) ||
            !expected_u64(*expected, "frame_id", request.expected.frame_id,
                          error) ||
            !expected_u64(*expected, "view_id", request.expected.view_id,
                          error)) {
            invalid(error);
            return answer;
        }
    }

    const CommandDescriptor* descriptor = find_command(request.command);
    if (!descriptor) {
        emit_terminal(id_text, command_text, std::nullopt,
                      Status::UnknownCommand, null_value(),
                      "command is not registered; call agent.commands");
        return answer;
    }
    std::set<std::string> declared;
    for (const Argument& argument : descriptor->arguments)
        declared.insert(argument.name);
    for (const auto& entry : request.arguments.obj) {
        if (!declared.count(entry.first)) {
            invalid("unknown argument '" + entry.first + "' for " +
                    request.command);
            return answer;
        }
    }
    for (const Argument& argument : descriptor->arguments) {
        const Value* value = unique_field(request.arguments, argument.name);
        if (!value && argument.required) {
            invalid("missing required argument '" + argument.name + "'");
            return answer;
        }
        if (value) {
            std::string type_error;
            if (!type_matches(*value, argument.type, type_error)) {
                invalid(type_error.empty()
                            ? "argument '" + argument.name + "' must be " +
                                  argument.type
                            : type_error);
                return answer;
            }
        }
    }

    const Context context = current_context();
    if (!expectations_match(request.expected, context)) {
        emit_terminal(id_text, command_text, std::nullopt,
                      Status::StaleRevision, null_value(),
                      "one or more expected revisions do not match current context");
        return answer;
    }
    const Availability availability = command_availability(*descriptor, context);
    if (!availability.available) {
        emit_terminal(id_text, command_text, std::nullopt,
                      availability.unavailable_status, null_value(),
                      availability.reason.empty() ? "command is unavailable"
                                                  : availability.reason);
        return answer;
    }
    if (!output_available_ || !sink_) {
        emit_terminal(id_text, command_text, std::nullopt, Status::NotReady,
                      null_value(), "MATTER_AGENT_RESULT_FILE is not configured");
        return answer;
    }
    if (pending_.size() >= kMaxPendingRequests) {
        emit_terminal(id_text, command_text, std::nullopt, Status::NotReady,
                      null_value(), "too many agent requests are pending");
        return answer;
    }

    pending_[request.request_id] =
        Pending{request.command, 0,
                now + std::chrono::milliseconds(request.timeout_ms)};
    answer.accepted = true;
    answer.request = std::move(request);
    return answer;
}

bool Protocol::attach_ticket(const std::string& request_id,
                             std::uint64_t ticket_id) {
    auto found = pending_.find(request_id);
    if (found == pending_.end() || found->second.ticket_id != 0 || ticket_id == 0)
        return false;
    found->second.ticket_id = ticket_id;
    return true;
}

bool Protocol::reject_accepted(const std::string& request_id, Status status,
                               const std::string& message) {
    auto found = pending_.find(request_id);
    if (found == pending_.end()) return false;
    const std::string command = found->second.command;
    const std::optional<std::uint64_t> ticket =
        found->second.ticket_id ? std::optional<std::uint64_t>(found->second.ticket_id)
                                : std::nullopt;
    pending_.erase(found);
    return emit_terminal(&request_id, &command, ticket, status, null_value(),
                         message);
}

bool Protocol::complete(const std::string& request_id, std::uint64_t ticket_id,
                        Status status, Value result,
                        const std::string& message) {
    auto found = pending_.find(request_id);
    if (found == pending_.end() || found->second.ticket_id != ticket_id ||
        ticket_id == 0)
        return false;
    const std::string command = found->second.command;
    pending_.erase(found);
    return emit_terminal(&request_id, &command, ticket_id, status,
                         std::move(result), message);
}

void Protocol::expire(Clock::time_point now) {
    std::vector<std::string> expired;
    for (const auto& entry : pending_) {
        if (now >= entry.second.deadline) expired.push_back(entry.first);
    }
    std::sort(expired.begin(), expired.end());
    for (const std::string& request_id : expired) {
        auto found = pending_.find(request_id);
        if (found == pending_.end()) continue;
        const std::string command = found->second.command;
        const std::optional<std::uint64_t> ticket =
            found->second.ticket_id
                ? std::optional<std::uint64_t>(found->second.ticket_id)
                : std::nullopt;
        pending_.erase(found);
        emit_terminal(&request_id, &command, ticket, Status::Timeout,
                      null_value(), "request deadline expired");
    }
}

bool Protocol::emit_terminal(const std::string* request_id,
                             const std::string* command,
                             std::optional<std::uint64_t> ticket_id,
                             Status status, Value result,
                             const std::string& message) {
    Value root = object_value();
    root.set("protocol", string_value("matter-agent"));
    root.set("version", number_value(kProtocolVersion));
    root.set("type", string_value("result"));
    root.set("request_id", request_id ? string_value(*request_id) : null_value());
    root.set("command", command ? string_value(*command) : null_value());
    root.set("ticket_id", ticket_id
                              ? string_value(std::to_string(*ticket_id))
                              : null_value());
    root.set("ok", bool_value(status == Status::Ok));
    root.set("code", string_value(to_string(status)));
    root.set("message", string_value(message));
    root.set("context", context_json(current_context()));
    root.set("result", std::move(result));
    std::string line = write_json_strict(root);

    if (line.size() > kMaxResultBytes) {
        root.set("ok", bool_value(false));
        root.set("code", string_value(to_string(Status::OutputTooLarge)));
        root.set("message", string_value("terminal result exceeds 1048576 byte limit"));
        root.set("result", null_value());
        line = write_json_strict(root);
    }
    if (!output_available_ || !sink_) {
        io_error_ = "MATTER_AGENT_RESULT_FILE is not configured";
        return false;
    }
    std::string error;
    if (!sink_(line, error)) {
        io_error_ = error.empty() ? "could not append agent result" : error;
        return false;
    }
    return true;
}

std::string Protocol::take_io_error() {
    std::string result = std::move(io_error_);
    io_error_.clear();
    return result;
}

}  // namespace viewer::agent
