#include "matter/json_doc.h"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace matter {
namespace jsondoc {
namespace {

class JsonParser {
public:
    explicit JsonParser(const std::string& text) : s_(text) {}

    bool parse(Value& out) {
        skip_ws();
        if (!parse_value(out)) return false;
        skip_ws();
        return true;  // trailing garbage tolerated
    }

private:
    const std::string& s_;
    size_t i_ = 0;

    void skip_ws() {
        while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) ++i_;
    }
    bool eof() const { return i_ >= s_.size(); }
    char peek() const { return i_ < s_.size() ? s_[i_] : '\0'; }

    bool parse_value(Value& out) {
        skip_ws();
        if (eof()) return false;
        char c = peek();
        if (c == '{') return parse_object(out);
        if (c == '[') return parse_array(out);
        if (c == '"') return parse_string_value(out);
        if (c == 't' || c == 'f') return parse_bool(out);
        if (c == 'n') return parse_null(out);
        return parse_number(out);
    }

    bool parse_object(Value& out) {
        out.kind = Value::Kind::Object;
        ++i_;  // '{'
        skip_ws();
        if (peek() == '}') { ++i_; return true; }
        while (true) {
            skip_ws();
            if (peek() != '"') return false;
            std::string key;
            if (!parse_raw_string(key)) return false;
            skip_ws();
            if (peek() != ':') return false;
            ++i_;
            Value val;
            if (!parse_value(val)) return false;
            out.obj.emplace_back(std::move(key), std::move(val));
            skip_ws();
            if (peek() == ',') { ++i_; continue; }
            if (peek() == '}') { ++i_; break; }
            return false;
        }
        return true;
    }

    bool parse_array(Value& out) {
        out.kind = Value::Kind::Array;
        ++i_;  // '['
        skip_ws();
        if (peek() == ']') { ++i_; return true; }
        while (true) {
            Value val;
            if (!parse_value(val)) return false;
            out.arr.push_back(std::move(val));
            skip_ws();
            if (peek() == ',') { ++i_; continue; }
            if (peek() == ']') { ++i_; break; }
            return false;
        }
        return true;
    }

    bool parse_raw_string(std::string& out) {
        if (peek() != '"') return false;
        ++i_;
        out.clear();
        while (!eof() && peek() != '"') {
            char c = s_[i_++];
            if (c == '\\' && !eof()) {
                char e = s_[i_++];
                switch (e) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    default: out += e; break;
                }
            } else {
                out += c;
            }
        }
        if (eof()) return false;
        ++i_;  // closing quote
        return true;
    }

    bool parse_string_value(Value& out) {
        out.kind = Value::Kind::String;
        return parse_raw_string(out.str);
    }

    bool parse_bool(Value& out) {
        if (s_.compare(i_, 4, "true") == 0) { out.kind = Value::Kind::Bool; out.b = true; i_ += 4; return true; }
        if (s_.compare(i_, 5, "false") == 0) { out.kind = Value::Kind::Bool; out.b = false; i_ += 5; return true; }
        return false;
    }

    bool parse_null(Value& out) {
        if (s_.compare(i_, 4, "null") == 0) { out.kind = Value::Kind::Null; i_ += 4; return true; }
        return false;
    }

    bool parse_number(Value& out) {
        size_t start = i_;
        if (peek() == '-') ++i_;
        while (!eof() && (std::isdigit(static_cast<unsigned char>(peek())) || peek() == '.' ||
                          peek() == 'e' || peek() == 'E' || peek() == '+' || peek() == '-'))
            ++i_;
        if (i_ == start) return false;
        const std::string token = s_.substr(start, i_ - start);
        const bool unsigned_integer = !token.empty() &&
            token.find_first_not_of("0123456789") == std::string::npos;
        if (unsigned_integer) {
            errno = 0;
            char* end = nullptr;
            const unsigned long long exact = std::strtoull(token.c_str(), &end, 10);
            constexpr std::uint64_t kMaxExactDoubleInteger = 9007199254740992ull;
            if (errno != ERANGE && end && *end == '\0' &&
                exact > kMaxExactDoubleInteger) {
                out.kind = Value::Kind::UInt64;
                out.uint64_value = static_cast<std::uint64_t>(exact);
                return true;
            }
        }
        out.kind = Value::Kind::Number;
        out.num = std::atof(token.c_str());
        return true;
    }
};

void write_json_escaped(const std::string& s, std::string& out) {
    out += '"';
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            default: out += c; break;
        }
    }
    out += '"';
}

}  // namespace

Value* Value::find(const std::string& key) {
    for (auto& kv : obj)
        if (kv.first == key) return &kv.second;
    return nullptr;
}

const Value* Value::find(const std::string& key) const {
    for (const auto& kv : obj)
        if (kv.first == key) return &kv.second;
    return nullptr;
}

Value& Value::set(const std::string& key, Value v) {
    kind = Kind::Object;
    for (auto& kv : obj) {
        if (kv.first == key) {
            kv.second = std::move(v);
            return kv.second;
        }
    }
    obj.emplace_back(key, std::move(v));
    return obj.back().second;
}

bool Value::erase(const std::string& key) {
    for (size_t i = 0; i < obj.size(); ++i) {
        if (obj[i].first == key) {
            obj.erase(obj.begin() + static_cast<long>(i));
            return true;
        }
    }
    return false;
}

bool parse_json(const std::string& text, Value& out) {
    JsonParser p(text);
    return p.parse(out);
}

void write_json(const Value& v, std::string& out) {
    switch (v.kind) {
        case Value::Kind::Null: out += "null"; break;
        case Value::Kind::Bool: out += v.b ? "true" : "false"; break;
        case Value::Kind::Number: {
            // Integral values print without a trailing ".0" so params like
            // {"seed": 3} round-trip as JS integers, not 3.0.
            double intpart;
            if (std::modf(v.num, &intpart) == 0.0 && std::fabs(v.num) < 1e15) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v.num));
                out += buf;
            } else {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%.9g", v.num);
                out += buf;
            }
            break;
        }
        case Value::Kind::UInt64:
            out += std::to_string(v.uint64_value);
            break;
        case Value::Kind::String: write_json_escaped(v.str, out); break;
        case Value::Kind::Array: {
            out += '[';
            for (size_t i = 0; i < v.arr.size(); ++i) {
                if (i) out += ',';
                write_json(v.arr[i], out);
            }
            out += ']';
            break;
        }
        case Value::Kind::Object: {
            out += '{';
            for (size_t i = 0; i < v.obj.size(); ++i) {
                if (i) out += ',';
                write_json_escaped(v.obj[i].first, out);
                out += ':';
                write_json(v.obj[i].second, out);
            }
            out += '}';
            break;
        }
    }
}

std::string write_json(const Value& v) {
    std::string out;
    write_json(v, out);
    return out;
}

}  // namespace jsondoc
}  // namespace matter
