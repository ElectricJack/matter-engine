#include "internal.h"
#include <charconv>
#include <cstring>

namespace matter::contracts {
const char *code_name(ErrorCode c) {
  switch (c) {
  case ErrorCode::invalid_input:
    return "invalid_input";
  case ErrorCode::unsupported_capability:
    return "unsupported_capability";
  case ErrorCode::unsupported_version:
    return "unsupported_version";
  case ErrorCode::missing_dependency:
    return "missing_dependency";
  case ErrorCode::cycle:
    return "cycle";
  case ErrorCode::resource_limit:
    return "resource_limit";
  case ErrorCode::cancelled:
    return "cancelled";
  case ErrorCode::stale_result:
    return "stale_result";
  case ErrorCode::backend_failure:
    return "backend_failure";
  }
  return "invalid_input";
}
bool ValidationResult::ok() const {
  for (const auto &d : diagnostics)
    if (d.severity == Severity::error)
      return false;
  return true;
}
namespace detail {
bool valid_utf8(std::string_view s) {
  size_t i = 0;
  while (i < s.size()) {
    const auto a = static_cast<unsigned char>(s[i++]);
    if (a < 0x80)
      continue;
    uint32_t cp = 0;
    size_t n = 0;
    if (a >= 0xc2 && a <= 0xdf) {
      cp = a & 31u;
      n = 1;
    } else if (a >= 0xe0 && a <= 0xef) {
      cp = a & 15u;
      n = 2;
    } else if (a >= 0xf0 && a <= 0xf4) {
      cp = a & 7u;
      n = 3;
    } else
      return false;
    if (n > s.size() - i)
      return false;
    for (size_t j = 0; j < n; ++j) {
      auto b = static_cast<unsigned char>(s[i++]);
      if ((b & 0xc0) != 0x80)
        return false;
      cp = (cp << 6) | (b & 63u);
    }
    if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) ||
        (n == 3 && cp < 0x10000) || cp > 0x10ffff ||
        (cp >= 0xd800 && cp <= 0xdfff))
      return false;
  }
  return true;
}
std::string hex64(uint64_t n) {
  std::string s(16, '0');
  for (int i = 15; i >= 0; --i) {
    s[static_cast<size_t>(i)] = "0123456789abcdef"[n & 15];
    n >>= 4;
  }
  return s;
}
namespace {
struct JsonError : std::runtime_error {
  ErrorCode code;
  JsonError(ErrorCode c, const char *text)
      : std::runtime_error(text), code(c) {}
};
[[noreturn]] void invalid(const char *text) {
  throw JsonError(ErrorCode::invalid_input, text);
}
[[noreturn]] void limit(const char *text) {
  throw JsonError(ErrorCode::resource_limit, text);
}
void utf8_append(std::string &s, uint32_t cp) {
  if (cp < 0x80)
    s.push_back(static_cast<char>(cp));
  else if (cp < 0x800) {
    s.push_back(static_cast<char>(0xc0 | (cp >> 6)));
    s.push_back(static_cast<char>(0x80 | (cp & 63)));
  } else if (cp < 0x10000) {
    s.push_back(static_cast<char>(0xe0 | (cp >> 12)));
    s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 63)));
    s.push_back(static_cast<char>(0x80 | (cp & 63)));
  } else {
    s.push_back(static_cast<char>(0xf0 | (cp >> 18)));
    s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 63)));
    s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 63)));
    s.push_back(static_cast<char>(0x80 | (cp & 63)));
  }
}
class Parser {
  std::string_view s;
  const ValidationLimits &limits;
  size_t i = 0;
  uint64_t nodes = 0;
  void space() {
    while (i < s.size() &&
           (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t'))
      ++i;
  }
  bool take(char c) {
    space();
    if (i < s.size() && s[i] == c) {
      ++i;
      return true;
    }
    return false;
  }
  uint32_t hex4() {
    uint32_t n = 0;
    for (int j = 0; j < 4; ++j) {
      if (i >= s.size())
        invalid("incomplete unicode escape");
      char c = s[i++];
      n <<= 4;
      if (c >= '0' && c <= '9')
        n |= static_cast<uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f')
        n |= static_cast<uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F')
        n |= static_cast<uint32_t>(c - 'A' + 10);
      else
        invalid("invalid unicode escape");
    }
    return n;
  }
  std::string string() {
    if (!take('"'))
      invalid("expected string");
    std::string r;
    while (i < s.size()) {
      char c = s[i++];
      if (c == '"') {
        if (!valid_utf8(r))
          invalid("invalid UTF-8");
        return r;
      }
      if (static_cast<unsigned char>(c) < 32)
        invalid("unescaped string control");
      if (c == '\\') {
        if (i == s.size())
          invalid("incomplete escape");
        c = s[i++];
        switch (c) {
        case '"':
        case '\\':
        case '/':
          r.push_back(c);
          break;
        case 'b':
          r.push_back('\b');
          break;
        case 'f':
          r.push_back('\f');
          break;
        case 'n':
          r.push_back('\n');
          break;
        case 'r':
          r.push_back('\r');
          break;
        case 't':
          r.push_back('\t');
          break;
        case 'u': {
          uint32_t cp = hex4();
          if (cp >= 0xd800 && cp <= 0xdbff) {
            if (i + 2 > s.size() || s[i] != '\\' || s[i + 1] != 'u')
              invalid("unpaired surrogate");
            i += 2;
            uint32_t b = hex4();
            if (b < 0xdc00 || b > 0xdfff)
              invalid("unpaired surrogate");
            cp = 0x10000 + ((cp - 0xd800) << 10) + (b - 0xdc00);
          } else if (cp >= 0xdc00 && cp <= 0xdfff)
            invalid("unpaired surrogate");
          utf8_append(r, cp);
          break;
        }
        default:
          invalid("unknown string escape");
        }
      } else
        r.push_back(c);
      if (r.size() > limits.max_string_bytes)
        limit("string byte budget exceeded");
    }
    invalid("unterminated string");
  }
  Value value(uint32_t depth) {
    if (depth > limits.max_depth)
      limit("JSON depth budget exceeded");
    if (++nodes > limits.max_value_nodes)
      limit("JSON value budget exceeded");
    space();
    if (i == s.size())
      invalid("expected JSON value");
    if (s[i] == '"')
      return Value(string());
    if (take('{')) {
      Value::Object o;
      if (take('}'))
        return o;
      do {
        auto k = string();
        if (!take(':'))
          invalid("expected colon");
        auto v = value(depth + 1);
        if (!o.emplace(std::move(k), std::move(v)).second)
          invalid("duplicate object key");
      } while (take(','));
      if (!take('}'))
        invalid("expected object end");
      return o;
    }
    if (take('[')) {
      Value::Array a;
      if (take(']'))
        return a;
      do {
        a.push_back(value(depth + 1));
      } while (take(','));
      if (!take(']'))
        invalid("expected array end");
      return a;
    }
    if (s.substr(i, 4) == "null") {
      i += 4;
      return nullptr;
    }
    if (s.substr(i, 4) == "true") {
      i += 4;
      return true;
    }
    if (s.substr(i, 5) == "false") {
      i += 5;
      return false;
    }
    const size_t begin = i;
    if (s[i] == '-')
      ++i;
    if (i >= s.size())
      invalid("incomplete number");
    if (s[i] == '0')
      ++i;
    else {
      if (s[i] < '1' || s[i] > '9')
        invalid("invalid number");
      while (i < s.size() && s[i] >= '0' && s[i] <= '9')
        ++i;
    }
    if (i < s.size() && s[i] == '.') {
      ++i;
      const auto d = i;
      while (i < s.size() && s[i] >= '0' && s[i] <= '9')
        ++i;
      if (d == i)
        invalid("missing decimal digits");
    }
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
      ++i;
      if (i < s.size() && (s[i] == '+' || s[i] == '-'))
        ++i;
      const auto d = i;
      while (i < s.size() && s[i] >= '0' && s[i] <= '9')
        ++i;
      if (d == i)
        invalid("missing exponent digits");
    }
    double n = 0;
    auto r = std::from_chars(s.data() + begin, s.data() + i, n,
                             std::chars_format::general);
    if (r.ec != std::errc{} || r.ptr != s.data() + i || !std::isfinite(n))
      invalid("number is outside finite binary64");
    return n;
  }

public:
  Parser(std::string_view text, const ValidationLimits &l)
      : s(text), limits(l) {}
  Value run() {
    if (s.size() > limits.max_document_bytes)
      limit("document byte budget exceeded");
    Value v = value(0);
    space();
    if (i != s.size())
      invalid("trailing JSON input");
    return v;
  }
};
// Exact binary64 decimal expansion. Integer decimal arithmetic avoids locale
// and C++/JS shortest-format disagreements. The largest finite expansion is
// 1076 bytes.
std::string exact_number(double n) {
  if (n == 0)
    return "0";
  uint64_t bits = 0;
  std::memcpy(&bits, &n, 8);
  const bool negative = (bits >> 63) != 0;
  const int encoded = static_cast<int>((bits >> 52) & 2047);
  uint64_t mantissa = bits & UINT64_C(0xfffffffffffff);
  int exponent = -1074;
  if (encoded) {
    mantissa |= UINT64_C(0x10000000000000);
    exponent = encoded - 1023 - 52;
  }
  std::string digits = std::to_string(mantissa);
  const int factor = exponent < 0 ? 5 : 2;
  const int count = exponent < 0 ? -exponent : exponent;
  for (int step = 0; step < count; ++step) {
    int carry = 0;
    for (size_t j = digits.size(); j-- > 0;) {
      int d = (digits[j] - '0') * factor + carry;
      digits[j] = static_cast<char>('0' + d % 10);
      carry = d / 10;
    }
    if (carry)
      digits.insert(digits.begin(), static_cast<char>('0' + carry));
  }
  if (exponent < 0) {
    const auto places = static_cast<size_t>(-exponent);
    if (digits.size() <= places)
      digits.insert(0, places + 1 - digits.size(), '0');
    digits.insert(digits.size() - places, 1, '.');
    while (digits.back() == '0')
      digits.pop_back();
    if (digits.back() == '.')
      digits.pop_back();
  }
  if (negative)
    digits.insert(digits.begin(), '-');
  return digits;
}
class Writer {
  const ValidationLimits &limits;
  std::string out;
  uint64_t nodes = 0;
  void append(std::string_view s) {
    if (s.size() > limits.max_document_bytes - out.size())
      limit("canonical byte budget exceeded");
    out.append(s);
  }
  void quoted(std::string_view s) {
    if (s.size() > limits.max_string_bytes)
      limit("string byte budget exceeded");
    if (!valid_utf8(s))
      invalid("invalid UTF-8");
    append("\"");
    for (unsigned char c : s) {
      if (c == '"')
        append("\\\"");
      else if (c == '\\')
        append("\\\\");
      else if (c < 32) {
        char e[] = {'\\',
                    'u',
                    '0',
                    '0',
                    "0123456789abcdef"[c >> 4],
                    "0123456789abcdef"[c & 15]};
        append(std::string_view(e, 6));
      } else {
        char a = static_cast<char>(c);
        append(std::string_view(&a, 1));
      }
    }
    append("\"");
  }
  void value(const Value &v, uint32_t depth) {
    if (depth > limits.max_depth)
      limit("value depth budget exceeded");
    if (++nodes > limits.max_value_nodes)
      limit("value count budget exceeded");
    if (std::holds_alternative<std::nullptr_t>(v.data))
      append("null");
    else if (auto boolean = std::get_if<bool>(&v.data))
      append(*boolean ? "true" : "false");
    else if (auto number = std::get_if<double>(&v.data)) {
      if (!std::isfinite(*number))
        invalid("nonfinite number");
      append(exact_number(*number));
    } else if (auto string = std::get_if<std::string>(&v.data))
      quoted(*string);
    else if (auto array = std::get_if<Value::Array>(&v.data)) {
      append("[");
      bool first = true;
      for (const auto &e : *array) {
        if (!first)
          append(",");
        first = false;
        value(e, depth + 1);
      }
      append("]");
    } else {
      append("{");
      bool first = true;
      for (const auto &e : std::get<Value::Object>(v.data)) {
        if (!first)
          append(",");
        first = false;
        quoted(e.first);
        append(":");
        value(e.second, depth + 1);
      }
      append("}");
    }
  }

public:
  explicit Writer(const ValidationLimits &l) : limits(l) {}
  std::string run(const Value &v) {
    value(v, 0);
    return out;
  }
};
} // namespace
} // namespace detail
ValidationResult parse_json(std::string_view s, Value &out,
                            const ValidationLimits &limits) {
  try {
    auto candidate = detail::Parser(s, limits).run();
    out = std::move(candidate);
    return {};
  } catch (const detail::JsonError &e) {
    return detail::failure(e.code, e.what());
  } catch (const std::bad_alloc &) {
    return detail::failure(ErrorCode::resource_limit, "JSON allocation failed");
  }
}
ValidationResult canonical_json(const Value &v, std::string &out,
                                const ValidationLimits &limits) {
  try {
    auto candidate = detail::Writer(limits).run(v);
    out = std::move(candidate);
    return {};
  } catch (const detail::JsonError &e) {
    return detail::failure(e.code, e.what());
  } catch (const std::bad_alloc &) {
    return detail::failure(ErrorCode::resource_limit,
                           "canonical allocation failed");
  }
}
ValidationResult validate_value(const Value &v,
                                const ValidationLimits &limits) {
  std::string bytes;
  return canonical_json(v, bytes, limits);
}
} // namespace matter::contracts
