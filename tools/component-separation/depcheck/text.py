"""Comment stripping and directive parsing shared by the include, import and
duplicate scans. Strings are kept (include and import specifiers live in them);
only comments are blanked, and line structure is preserved so findings keep
their line numbers."""

from __future__ import annotations

import re
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

# File reads are latency-bound on network and WSL-mounted drives; a small pool
# overlaps them. This is I/O only and never runs a compiler.
READ_WORKERS = 8

C_INCLUDE = re.compile(r'^[ \t]*#[ \t]*(?:include|import)[ \t]*([<"])([^>"\n]+)[>"]', re.MULTILINE)

# Specifier-bearing JavaScript forms. Comments are removed first; a property
# call such as ``builder.require(x)`` is not a CommonJS require.
JS_IMPORT_PATTERNS = (
    re.compile(r"\bimport\s+(?:[\w$*{}\s,]+?\s+from\s*)?(['\"])([^'\"\n]+)\1"),
    re.compile(r"\bexport\s+(?:\*(?:\s+as\s+[\w$]+)?|\{[^}]*\})\s*from\s*(['\"])([^'\"\n]+)\1"),
    re.compile(r"\bimport\s*\(\s*(['\"])([^'\"\n]+)\1"),
    re.compile(r"(?<![.\w$#])require\s*\(\s*(['\"])([^'\"\n]+)\1"),
)

C_FAMILY = (".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl")
JS_FAMILY = (".mjs", ".js", ".cjs", ".mts", ".ts")


def family(path: str) -> str | None:
    lower = path.lower()
    if lower.endswith(C_FAMILY):
        return "c"
    if lower.endswith(JS_FAMILY):
        return "js"
    return None


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def read_many(paths: list[Path]) -> list[str]:
    """``read_text`` for each path, in order, overlapping the reads."""
    if len(paths) < 2 * READ_WORKERS:
        return [read_text(path) for path in paths]
    with ThreadPoolExecutor(max_workers=READ_WORKERS) as pool:
        return list(pool.map(read_text, paths))


def strip_comments(text: str, js: bool = False) -> str:
    """Blank // and /* */ comments while keeping strings and newlines."""
    out: list[str] = []
    i, n = 0, len(text)
    quotes = ("'", '"', "`") if js else ("'", '"')
    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if ch == "/" and nxt == "/":
            end = text.find("\n", i)
            i = n if end < 0 else end
            continue
        if ch == "/" and nxt == "*":
            end = text.find("*/", i + 2)
            chunk = text[i:n if end < 0 else end + 2]
            out.append("\n" * chunk.count("\n"))
            i = n if end < 0 else end + 2
            continue
        if not js and ch == "R" and nxt == '"' and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == "_")):
            open_paren = text.find("(", i + 2)
            if 0 <= open_paren - (i + 2) <= 16:
                delimiter = ")" + text[i + 2:open_paren] + '"'
                end = text.find(delimiter, open_paren)
                end = n if end < 0 else end + len(delimiter)
                out.append(text[i:end])
                i = end
                continue
        if ch in quotes:
            j = i + 1
            while j < n and text[j] != ch:
                if text[j] == "\\":
                    j += 1
                elif text[j] == "\n" and ch != "`":
                    break  # unterminated literal; resynchronise at the line end
                j += 1
            out.append(text[i:j + 1])
            i = j + 1
            continue
        out.append(ch)
        i += 1
    return "".join(out)


def c_includes(text: str) -> list[tuple[int, str, str]]:
    """[(line, delimiter, spec)] for every #include/#import directive."""
    stripped = strip_comments(text)
    found = []
    for match in C_INCLUDE.finditer(stripped):
        line = stripped.count("\n", 0, match.start()) + 1
        found.append((line, match.group(1), match.group(2).strip()))
    return found


def js_specifiers(text: str) -> list[tuple[int, str]]:
    stripped = strip_comments(text, js=True)
    found = {}
    for pattern in JS_IMPORT_PATTERNS:
        for match in pattern.finditer(stripped):
            line = stripped.count("\n", 0, match.start()) + 1
            found[(line, match.group(2))] = None
    return sorted(found)


STANDARD_HEADERS = frozenset("""
algorithm any array atomic barrier bit bitset cassert ccomplex cctype cerrno cfenv cfloat charconv chrono
cinttypes ciso646 climits clocale cmath codecvt compare complex concepts condition_variable coroutine csetjmp
csignal cstdalign cstdarg cstdbool cstddef cstdint cstdio cstdlib cstring ctgmath ctime cuchar cwchar cwctype
deque exception execution expected filesystem format forward_list fstream functional future initializer_list
iomanip ios iosfwd iostream istream iterator latch limits list locale map memory memory_resource mutex new
numbers numeric optional ostream queue random ranges ratio regex scoped_allocator semaphore set shared_mutex
source_location span sstream stack stdexcept stop_token streambuf string string_view syncstream system_error
thread tuple type_traits typeindex typeinfo unordered_map unordered_set utility valarray variant vector version
assert.h complex.h ctype.h errno.h fenv.h float.h inttypes.h iso646.h limits.h locale.h math.h setjmp.h
signal.h stdalign.h stdarg.h stdatomic.h stdbool.h stddef.h stdint.h stdio.h stdlib.h stdnoreturn.h string.h
tgmath.h threads.h time.h uchar.h wchar.h wctype.h
""".split())
