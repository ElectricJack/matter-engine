#include "../include/module_resolver.h"
#include <cctype>

namespace module_resolver {

// A deliberately small, dependency-free scanner. It strips line comments and
// block comments, and blanks the *bodies* of string/template literals so that an
// "import"/"from" token embedded in a string or comment can never match. The one
// exception: a string literal that is the target of `import ... from '<spec>'`
// (or a bare side-effect `import '<spec>'`) is preserved verbatim, because that is
// the specifier we want to extract. We decide that by looking at the preceding
// non-whitespace code token at the moment the opening quote is seen.
static bool prev_token_is(const std::string& out, const char* tok) {
    // Walk back over whitespace in the already-emitted output, then compare the
    // immediately-preceding identifier-like token to `tok`.
    size_t j = out.size();
    while (j > 0 && std::isspace((unsigned char)out[j - 1])) --j;
    size_t end = j;
    auto is_ident = [](char c) { return std::isalnum((unsigned char)c) || c == '_' || c == '$'; };
    while (j > 0 && is_ident(out[j - 1])) --j;
    std::string word = out.substr(j, end - j);
    return word == tok;
}

static std::string strip_comments_and_strings(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    enum { CODE, LINE_COMMENT, BLOCK_COMMENT, SQ, DQ, TICK } st = CODE;
    bool keep_string = false;  // true while inside a string that is a from/import target
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        char n = (i + 1 < s.size()) ? s[i + 1] : '\0';
        switch (st) {
        case CODE:
            if (c == '/' && n == '/') { st = LINE_COMMENT; out += ' '; out += ' '; ++i; }
            else if (c == '/' && n == '*') { st = BLOCK_COMMENT; out += ' '; out += ' '; ++i; }
            else if (c == '\'' || c == '"') {
                // A from/import target keeps its body; any other string is blanked.
                keep_string = prev_token_is(out, "from") || prev_token_is(out, "import");
                st = (c == '\'') ? SQ : DQ;
                out += c;
            }
            else if (c == '`')  { st = TICK; out += ' '; }
            else out += c;
            break;
        case LINE_COMMENT:
            if (c == '\n') { st = CODE; out += '\n'; } else out += ' ';
            break;
        case BLOCK_COMMENT:
            if (c == '*' && n == '/') { st = CODE; out += ' '; out += ' '; ++i; }
            else out += (c == '\n') ? '\n' : ' ';
            break;
        case SQ: if (c == '\\') { ++i; } else if (c == '\'') { st = CODE; out += '\''; } else out += keep_string ? c : ' '; break;
        case DQ: if (c == '\\') { ++i; } else if (c == '"')  { st = CODE; out += '"';  } else out += keep_string ? c : ' '; break;
        case TICK: if (c == '\\') { ++i; } else if (c == '`') { st = CODE; out += ' '; } else out += ' '; break;
        }
    }
    return out;
}

std::vector<std::string> parse_import_specifiers(const std::string& source) {
    // After stripping, a real specifier appears as: from <ws> <quote> <chars> <quote>
    // where the surrounding context started with the `import` keyword. We match the
    // keyword to avoid picking up `export ... from`.
    const std::string clean = strip_comments_and_strings(source);
    std::vector<std::string> out;
    size_t i = 0;
    auto is_ident = [](char c) { return std::isalnum((unsigned char)c) || c == '_' || c == '$'; };
    while (i < clean.size()) {
        // find next "import" token at a word boundary
        size_t k = clean.find("import", i);
        if (k == std::string::npos) break;
        bool lhs_ok = (k == 0) || !is_ident(clean[k - 1]);
        size_t after = k + 6;
        bool rhs_ok = (after >= clean.size()) || !is_ident(clean[after]);
        if (!lhs_ok || !rhs_ok) { i = k + 6; continue; }
        // from here, find the next `from` then the quoted specifier, but stop at ';'
        size_t semi = clean.find(';', after);
        size_t f = clean.find("from", after);
        if (f == std::string::npos || (semi != std::string::npos && f > semi)) {
            // bare `import 'shared-lib/x';` (side-effect import) — quote follows directly
            size_t q = clean.find_first_of("'\"", after);
            if (q != std::string::npos && (semi == std::string::npos || q < semi)) {
                char qc = clean[q];
                size_t e = clean.find(qc, q + 1);
                if (e != std::string::npos) { out.push_back(clean.substr(q + 1, e - q - 1)); i = e + 1; continue; }
            }
            i = after; continue;
        }
        size_t q = clean.find_first_of("'\"", f + 4);
        if (q == std::string::npos) { i = f + 4; continue; }
        char qc = clean[q];
        size_t e = clean.find(qc, q + 1);
        if (e == std::string::npos) { i = q + 1; continue; }
        out.push_back(clean.substr(q + 1, e - q - 1));
        i = e + 1;
    }
    return out;
}

} // namespace module_resolver
