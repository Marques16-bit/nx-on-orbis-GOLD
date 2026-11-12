#!/usr/bin/env bash
# Host test of the JIT direct table (patches/eden-jit/0002-*.py). The script edits six Eden files that are not in this repository; this
# test runs it on copies of the Eden master versions of the places it edits (tests/jit_pack/fixtures/), checks that it refuses to touch
# anything when one of its anchors is missing, and compiles and runs the page table half (common/page_table.h/.cpp as the script leaves
# them) on a PC with the console's lazy vector stood in (tests/jit_pack/direct_shim). The Xbyak emitters cannot be built here: they are
# checked by structure only. Run from the repository root:  bash tests/jit_direct_test.sh
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
SCRIPT="$ROOT/patches/eden-jit/0002-JIT-direct-page-table-copy.py"
FX="$ROOT/tests/jit_pack/fixtures"
D=src/dynarmic/src/dynarmic/backend/x64
pass=0
ok() { pass=$((pass + 1)); }
fail() { echo "FAIL: $*"; exit 1; }

mktree() { # $1 = directory
    mkdir -p "$1/src/common" "$1/src/core" "$1/$D"
    cp "$FX/page_table.h" "$FX/page_table.cpp" "$1/src/common/"
    cp "$FX/memory_excerpt.cpp" "$1/src/core/memory.cpp"
    cp "$FX/emit_x64_memory.h" "$1/$D/emit_x64_memory.h"
    cp "$FX/slow_lookup_excerpt.inc" "$1/$D/emit_x64_memory.cpp.inc"
    cp "$FX/eden_page_lookup_excerpt.cpp" "$1/$D/a64_emit_x64_memory.cpp"
}
sums() { (cd "$1" && find . -type f | sort | xargs sha256sum); }

# 1. The script applies to the fixtures, once.
mktree "$WORK/ok"
python3 -I "$SCRIPT" "$WORK/ok" >"$WORK/out.txt" 2>&1 || { cat "$WORK/out.txt"; fail "the script does not apply to the Eden master fixtures"; }
grep -q "6 files edited" "$WORK/out.txt" || fail "unexpected output: $(cat "$WORK/out.txt")"; ok
if python3 -I "$SCRIPT" "$WORK/ok" >"$WORK/again.txt" 2>&1; then fail "the script applied a second time"; fi; ok

# 2. A missing anchor: exit status 1 and not one file changed. Each edit is broken in turn by renaming a line it needs.
break_one() { # file, sed expression
    rm -rf "$WORK/neg"; mktree "$WORK/neg"
    sed -i "$2" "$WORK/neg/$1"
    cmp -s "$WORK/neg/$1" <(mktree "$WORK/neg2" 2>/dev/null; cat "$WORK/neg2/$1") && { rm -rf "$WORK/neg2"; fail "the sed did not change $1 ($2)"; }
    rm -rf "$WORK/neg2"
    before="$(sums "$WORK/neg")"
    if python3 -I "$SCRIPT" "$WORK/neg" >/dev/null 2>"$WORK/neg.err"; then fail "applied although '$2' broke $1"; fi
    [ "$before" = "$(sums "$WORK/neg")" ] || fail "files changed although '$2' broke $1"
    grep -q "does not fit this Eden" "$WORK/neg.err" || fail "no explanation for '$2' on $1"
    ok
}
break_one src/common/page_table.h 's/data_raw.fetch_or(0b111)/data_raw.fetch_or(0b11)/'
break_one src/common/page_table.h 's/inline void Store(/inline void Put(/'
break_one src/common/page_table.h 's/SparseLargeVector<PageEntryData> entries;/SparseLargeVector<PageEntryData> items;/'
break_one src/common/page_table.cpp 's/entries.ResizeAndClear(num_page_table_entries)/entries.ResizeAndClear(n)/'
break_one src/common/page_table.cpp 's/~PageTable() noexcept = default/~PageTable() noexcept {}/'
break_one src/core/memory.cpp 's/page_table.entries.ZeroRegion(base, end);/page_table.entries.ZeroRegion(base, end, 0);/'
break_one "$D/emit_x64_memory.h" 's/constexpr size_t page_table_const_mask/constexpr size_t page_table_const_msk/'
break_one "$D/emit_x64_memory.h" 's/if (ctx.conf.page_table_log2_stride > 3) {/if (ctx.conf.page_table_log2_stride >= 4) {/'
break_one "$D/emit_x64_memory.cpp.inc" 's/code.mov(page, code.qword\[r14 + idx \* 8\]);/code.mov(page, code.qword[r14 + idx * 4]);/'
break_one "$D/a64_emit_x64_memory.cpp" 's/code.mov(host, code.qword\[r14 + host \* 8\]);/code.mov(host, code.qword[r14 + host * 4]);/'
break_one "$D/a64_emit_x64_memory.cpp" 's/^}  \/\/ namespace Dynarmic::Backend::X64$/}/'
# a Dynarmic file missing altogether
rm -rf "$WORK/neg"; mktree "$WORK/neg"; rm "$WORK/neg/$D/emit_x64_memory.h"
if python3 -I "$SCRIPT" "$WORK/neg" >/dev/null 2>&1; then fail "applied without emit_x64_memory.h"; fi; ok

# 3. The edited emitters: by structure (they are Xbyak code, built only by the CI).
E="$WORK/ok/$D"
[ "$(grep -c 'PS4DirectOk(' "$E/emit_x64_memory.h")" -eq 2 ] || fail "PS4DirectOk in emit_x64_memory.h"; ok   # the definition and the A64 lookup
[ "$(grep -c 'PS4DirectOk(' "$E/emit_x64_memory.cpp.inc")" -eq 1 ] || fail "PS4DirectOk in the slow lookup"; ok
[ "$(grep -c 'PS4DirectOk(' "$E/a64_emit_x64_memory.cpp")" -eq 1 ] || fail "PS4DirectOk in the group lookup"; ok
grep -q '^int ps4_direct_present = 1;' "$E/a64_emit_x64_memory.cpp" || fail "ps4_direct_present"; ok
# In the A64 lookup the direct branch comes after the range check and before the decoding, and returns page + vaddr.
awk '/EmitVAddrLookup<A64EmitContext>\(/ {on=1} on' "$E/emit_x64_memory.h" >"$WORK/a64lookup.txt"
l_range="$(grep -n 'code.jnz(abort' "$WORK/a64lookup.txt" | head -n 1 | cut -d: -f1)"
l_direct="$(grep -n 'if (direct) {' "$WORK/a64lookup.txt" | head -n 1 | cut -d: -f1)"
l_marked="$(grep -n 'page_table_marked_bit' "$WORK/a64lookup.txt" | head -n 1 | cut -d: -f1)"
[ -n "$l_range" ] && [ -n "$l_direct" ] && [ -n "$l_marked" ] && [ "$l_range" -lt "$l_direct" ] && [ "$l_direct" -lt "$l_marked" ] || fail "order of the direct branch in the A64 lookup"; ok
sed -n "${l_direct},$((l_direct + 8))p" "$WORK/a64lookup.txt" | grep -q 'return page + vaddr;' || fail "the direct branch does not return page + vaddr"; ok
# The group lookup leaves the decoding out when direct; the slot is read from the mirror, not from r14.
grep -q 'if (!direct && conf.page_table_marked_bit)' "$E/a64_emit_x64_memory.cpp" || fail "marked bit still tested when direct"; ok
grep -q 'ps4_direct_mirror_base' "$E/a64_emit_x64_memory.cpp" || fail "group lookup does not use the mirror"; ok
# Nothing of the direct table is outside #ifdef __ORBIS__ in Eden's common code.
grep -c '__ORBIS__' "$WORK/ok/src/common/page_table.h" "$WORK/ok/src/common/page_table.cpp" "$WORK/ok/src/core/memory.cpp" >/dev/null; ok
awk '/ps4_direct/ && !/^#/ {print FILENAME ":" FNR}' /dev/null
python3 - "$WORK/ok" <<'PY' || fail "direct table code outside #ifdef __ORBIS__"
import re, sys, pathlib
root = pathlib.Path(sys.argv[1])
for rel in ("src/common/page_table.h", "src/common/page_table.cpp", "src/core/memory.cpp"):
    depth = 0
    for n, line in enumerate((root / rel).read_text().splitlines(), 1):
        s = line.strip()
        if s.startswith("#ifdef __ORBIS__"):
            depth += 1
        elif s.startswith("#endif") and depth:
            depth -= 1
        elif depth == 0 and re.search(r"ps4_direct|DirectSlot|SyncDirect|ZeroDirect|direct_entries|direct_live", line):
            print(f"{rel}:{n}: {line}")
            sys.exit(1)
PY
ok

# 4. The page table half, built and run on the PC.
SH="$ROOT/tests/jit_pack/direct_shim"
g++ -std=c++20 -Wall -Wextra -O1 -fsanitize=address,undefined -D__ORBIS__ -I"$WORK/ok/src" -I"$SH" \
    "$ROOT/tests/jit_pack/direct_test.cpp" "$WORK/ok/src/common/page_table.cpp" -o "$WORK/direct_test" -pthread
"$WORK/direct_test"
g++ -std=c++20 -O1 -fsanitize=thread -D__ORBIS__ -I"$WORK/ok/src" -I"$SH" \
    "$ROOT/tests/jit_pack/direct_test.cpp" "$WORK/ok/src/common/page_table.cpp" -o "$WORK/direct_test_tsan" -pthread
echo "(thread sanitizer run: its report would be above this line)"
TSAN_OPTIONS="halt_on_error=0 report_signal_unsafe=0" "$WORK/direct_test_tsan" || true
echo "jit_direct_test: $pass structure checks passed"
