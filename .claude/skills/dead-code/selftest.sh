#!/usr/bin/env bash
# Ground-truth check for dead_code.py.
#
# Builds a two-TU program with exactly one provably-unreferenced function and
# asserts the analyser finds it (and only it).  Also re-verifies the premise
# this whole skill rests on: that GNU ld's --gc-sections does NOT drop .text$
# sections on this toolchain.  If that ever starts working, the linker becomes
# the better tool and this skill should be revisited.
set -u

export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
: "${TMPDIR:=${LOCALAPPDATA:-C:/Users/$USER/AppData/Local}/Temp}"
export TMPDIR TMP="$TMPDIR" TEMP="$TMPDIR"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="$(mktemp -d "${TMPDIR}/deadcode-selftest-XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
cd "$WORK" || exit 1

cat > lib.cpp <<'EOF'
#include <cstdio>
void used_fn(){ printf("used\n"); }
void definitely_dead_fn(){ printf("dead\n"); }
int dead_data_blob[4096] = {1};
EOF
cat > m.cpp <<'EOF'
void used_fn();
int main(){ used_fn(); return 0; }
EOF

g++ -O2 -ffunction-sections -fdata-sections -c lib.cpp -o lib.o || exit 1
g++ -O2 -ffunction-sections -fdata-sections -c m.cpp   -o m.o   || exit 1
g++ m.o lib.o -o t.exe -Wl,--gc-sections || exit 1

fail=0

# --- premise: ld keeps the dead .text$ section ---------------------------
if nm t.exe | grep -q "definitely_dead_fn"; then
  echo "OK   premise holds: --gc-sections did NOT prune the dead .text\$ section"
else
  echo "NOTE --gc-sections now prunes .text\$ sections on this toolchain."
  echo "     The linker can answer this question directly -- revisit SKILL.md."
fi

# --- the analyser finds exactly the planted function ---------------------
printf 'lib.o\nm.o\n' > inputs.txt
out="$(python3 "$HERE/dead_code.py" --inputs inputs.txt --repo "$WORK" \
       --out res.json --no-xref 2>&1)"
echo "$out"

echo "$out" | grep -q "SANITY unreachable-with-live-caller (must be 0): 0" || {
  echo "FAIL sanity check did not pass"; fail=1; }
echo "$out" | grep -qE "authored functions +: 1\b" || {
  echo "FAIL expected exactly 1 authored dead function"; fail=1; }

# NB: `python3 - <<EOF` does not work under MSYS -- the bare `-` gets path
# translated and python reports "<stdin> is a directory".  Use a real file.
cat > check.py <<'PY'
import json, sys
rows = [r for r in json.load(open(sys.argv[1]))["rows"] if r["authored"]]
names = [r["bare"] for r in rows]
shapes = {r["bare"]: r["shape"] for r in rows}
if names != ["definitely_dead_fn"]:
    print("FAIL expected ['definitely_dead_fn'], got %r" % (names,)); sys.exit(1)
if shapes["definitely_dead_fn"] != "orphan":
    print("FAIL expected shape orphan, got %s" % shapes["definitely_dead_fn"]); sys.exit(1)
print("OK   analyser flagged definitely_dead_fn (orphan) and nothing else")
PY
python3 check.py res.json || fail=1

[ "$fail" = 0 ] && echo "SELFTEST PASS" || echo "SELFTEST FAIL"
exit "$fail"
