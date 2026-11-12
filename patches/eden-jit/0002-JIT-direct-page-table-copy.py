#!/usr/bin/env python3
# nx-on-orbis 0.9.3, JIT pack part 2: the "direct table".
#
# Every guest memory access the JIT compiles walks Eden's page table entry: load the 8-byte entry, test the marked bit, mask
# off the attribute bits, shift left and arithmetic-shift right to sign-extend, test for zero. This part keeps a second table
# beside it, one 8-byte slot per entry holding the entry already decoded (host - guest, 0 = take the callbacks), and the JIT
# loads that slot and tests it for zero: four instructions fewer per access. Eden4 does the same with its own table layout.
#
# This is a script of anchored edits and not a diff, because it has to fit an Eden commit that is not at hand while this is
# written: each edit needs only one line (or a few) to be there; if any anchor is missing or not unique nothing is written and
# the exit status is 1 (scripts/apply-jit-pack.sh then builds without this part). Usage: 0002-...py <Eden checkout>
#
# Whatever it changes is under `#ifdef __ORBIS__`. The decoded slot of an entry is written only by PageEntryData itself
# (Store, MarkRasterizerCached) and cleared where the table is zeroed (core/memory.cpp), always zero first when the entry
# becomes slow, always checked again after the entry changed (SyncDirect), so a slot is either 0 or the entry's pointer.
import pathlib
import re
import sys


class Miss(Exception):
    pass


def once(text, pattern, repl, what, regex=False, flags=0):
    """Replaces the single match of pattern (a literal string unless regex) in text."""
    if regex:
        found = list(re.finditer(pattern, text, flags))
        if len(found) != 1:
            raise Miss(f"{what}: {len(found)} matches, wanted 1")
        m = found[0]
        return text[: m.start()] + (repl(m) if callable(repl) else repl) + text[m.end():]
    n = text.count(pattern)
    if n != 1:
        raise Miss(f"{what}: {n} matches, wanted 1")
    return text.replace(pattern, repl if not callable(repl) else repl(None), 1)


# ---------------------------------------------------------------------------------------------------------------------------
# src/common/page_table.h

PT_EXTERNS = '''#ifdef __ORBIS__
// nx-on-orbis 0.9.3 (JIT direct table, see patches/eden-jit/0002-*.py). The definitions are the frontend's (frontend/jit_pack.h).
extern "C" {
extern bool ps4_direct_table;                      // settings.txt jit_direct_table=on; set before the first page table is sized
extern bool ps4_direct_live;                       // one page table has a direct copy: the JIT may read it
extern std::uintptr_t ps4_direct_entries_base;     // the entries of that table
extern std::uintptr_t ps4_direct_entries_bytes;
extern std::uintptr_t ps4_direct_mirror_base;      // its direct copy: the slot of entry i is at mirror_base + 8 * i
}
#endif

'''

PT_STORE = '''inline void Store(bool marked, PageType type, u16 block, uintptr_t pointer) noexcept {
#ifdef __ORBIS__
            std::atomic<u64>* const direct = DirectSlot();
            if (direct) {
                direct->store(0); // slow first: while the entry changes, the JIT takes the callbacks
            }
#endif
            data_raw.store(std::bit_cast<u64>(Data{marked, type, block, pointer}));
#ifdef __ORBIS__
            if (direct) {
                SyncDirect(*direct);
            }
#endif
        }'''

PT_MARK = '''inline void MarkRasterizerCached() noexcept {
#ifdef __ORBIS__
            std::atomic<u64>* const direct = DirectSlot();
            if (direct) {
                direct->store(0);
            }
#endif
            data_raw.fetch_or(0b111);
#ifdef __ORBIS__
            if (direct) {
                SyncDirect(*direct);
            }
#endif
        }'''

PT_PRIVATE = '''private:
#ifdef __ORBIS__
        /// The direct slot of this entry, or null when this entry is not in the table that has a direct copy.
        [[nodiscard]] std::atomic<u64>* DirectSlot() noexcept {
            const std::uintptr_t offset = reinterpret_cast<std::uintptr_t>(this) - ps4_direct_entries_base;
            return offset < ps4_direct_entries_bytes ? reinterpret_cast<std::atomic<u64>*>(ps4_direct_mirror_base + offset) : nullptr;
        }

        /// Writes the slot from the entry, and again if the entry changed while it was written (another thread storing or
        /// marking the same entry): the last write of a slot is always made from an entry that did not change after it.
        void SyncDirect(std::atomic<u64>& slot) noexcept {
            for (;;) {
                const u64 raw = data_raw.load();
                slot.store(static_cast<u64>(ExtractPointer(std::bit_cast<Data>(raw)))); // 0 when marked or empty
                if (data_raw.load() == raw) {
                    return;
                }
            }
        }

#endif
'''

PT_MEMBERS = '''
#ifdef __ORBIS__
    /// nx-on-orbis 0.9.3 (JIT direct table): slot i holds entries[i] decoded, see PageEntryData::SyncDirect. Allocated only for
    /// the first table sized while ps4_direct_table is on; ps4_direct_* name it for the entries and for the JIT.
    SparseLargeVector<std::atomic<u64>> direct_entries;
    bool direct_live{};

    /// Zeroes the slots of entries [start, end) that have memory (the entries' own zeroing skips the ones that do not, and so
    /// does this: it must not give memory to a table that is mostly empty). Call it before the entries are zeroed.
    void ZeroDirect(std::size_t start, std::size_t end) noexcept {
        if (!direct_live || start >= end) {
            return;
        }
        const std::size_t per_page = static_cast<std::size_t>(HostPageSize) / sizeof(PageEntryData);
        auto* const mirror = reinterpret_cast<u64*>(ps4_direct_mirror_base);
        for (std::size_t i = start; i < end;) {
            const std::size_t stop = std::min(end, (i / per_page + 1) * per_page);
            if (&entries.GetOrDefault(i) == &entries.data()[i]) { // the entries' page has memory
                std::memset(mirror + i, 0, (stop - i) * sizeof(u64));
            }
            i = stop;
        }
    }
#endif
'''


def edit_page_table_h(t):
    t = once(t, "#include <atomic>\n", "#include <algorithm>\n#include <atomic>\n#include <cstdint>\n#include <cstring>\n", "page_table.h includes")
    t = once(t, "namespace Common {\n", PT_EXTERNS + "namespace Common {\n", "page_table.h namespace")
    t = once(
        t,
        r"inline void Store\(bool marked, PageType type, u16 block, uintptr_t pointer\) noexcept \{\s*"
        r"data_raw\.store\(std::bit_cast<u64>\(Data\{marked, type, block, pointer\}\)\);\s*\}",
        PT_STORE, "PageEntryData::Store", regex=True)
    t = once(
        t,
        r"inline void MarkRasterizerCached\(\) noexcept \{\s*data_raw\.fetch_or\(0b111\);\s*\}",
        PT_MARK, "PageEntryData::MarkRasterizerCached", regex=True)
    t = once(t, r"private:(\s*\n\s*)std::atomic<u64> data_raw;", lambda m: PT_PRIVATE + m.group(1).lstrip("\n") + "std::atomic<u64> data_raw;",
             "PageEntryData::data_raw", regex=True)
    t = once(t, r"(SparseLargeVector<PageEntryData> entries;\n)", lambda m: m.group(1) + PT_MEMBERS, "PageTable::entries", regex=True)
    return t


# ---------------------------------------------------------------------------------------------------------------------------
# src/common/page_table.cpp

PC_DTOR = '''PageTable::~PageTable() noexcept {
#ifdef __ORBIS__
    if (direct_live) { // the memory behind the table's slots goes away with it: nothing may keep pointing at it
        ps4_direct_live = false;
        ps4_direct_entries_base = 0;
        ps4_direct_entries_bytes = 0;
        ps4_direct_mirror_base = 0;
    }
#endif
}'''

PC_RESIZE = '''entries.ResizeAndClear(num_page_table_entries);
#ifdef __ORBIS__
    // nx-on-orbis 0.9.3 (JIT direct table): the first table sized with the option on gets the direct copy.
    if (ps4_direct_table && (direct_live || !ps4_direct_live)) {
        direct_entries.ResizeAndClear(num_page_table_entries);
        ps4_direct_entries_base = reinterpret_cast<std::uintptr_t>(entries.data());
        ps4_direct_entries_bytes = static_cast<std::uintptr_t>(num_page_table_entries * sizeof(PageEntryData));
        ps4_direct_mirror_base = reinterpret_cast<std::uintptr_t>(direct_entries.data());
        direct_live = true;
        ps4_direct_live = true;
    }
#endif'''


def edit_page_table_cpp(t):
    t = once(t, "PageTable::~PageTable() noexcept = default;", PC_DTOR, "PageTable::~PageTable")
    t = once(t, "entries.ResizeAndClear(num_page_table_entries);", PC_RESIZE, "PageTable::Resize")
    return t


# ---------------------------------------------------------------------------------------------------------------------------
# src/core/memory.cpp: the one place that zeroes entries

MEM_ZERO_OLD = "page_table.entries.ZeroRegion(base, end);"
MEM_ZERO_NEW = '''#ifdef __ORBIS__
            page_table.ZeroDirect(base, end);
#endif
            page_table.entries.ZeroRegion(base, end);'''


def edit_memory_cpp(t):
    return once(t, MEM_ZERO_OLD, MEM_ZERO_NEW, "Memory::Impl::MapPages ZeroRegion")


# ---------------------------------------------------------------------------------------------------------------------------
# Dynarmic: src/dynarmic/src/dynarmic/backend/x64/emit_x64_memory.h

H_DECLS = '''#include <cstdint>

// nx-on-orbis 0.9.3 (JIT direct table, patches/eden-jit/0002-*.py): see common/page_table.h. Definitions: frontend/jit_pack.h
// (weak) and a64_emit_x64_memory.cpp (ps4_direct_present, ps4_direct_emitted).
extern "C" {
extern bool ps4_direct_live;
extern std::uintptr_t ps4_direct_entries_base;
extern std::uintptr_t ps4_direct_mirror_base;
extern unsigned long long ps4_direct_emitted;
}

'''

H_OK = '''
// The JIT may read the decoded slot instead of decoding the entry when this page table has one (ps4_direct_live, and conf.page_table
// is that table), the lookup is absolute-offset with plain 8-byte entries and no mirroring. A slot is 0 or host - guest: the same
// 0 as "unmapped or marked" that the decoding leaves.
template<typename Config>
inline bool PS4DirectOk([[maybe_unused]] const Config& conf) {
    if constexpr (requires { conf.page_table_address_space_bits; conf.page_table_log2_stride; conf.silently_mirror_page_table; }) {
        return ps4_direct_live && conf.page_table != nullptr
            && reinterpret_cast<std::uintptr_t>(conf.page_table) == ps4_direct_entries_base
            && conf.absolute_offset_page_table && !conf.silently_mirror_page_table && conf.page_table_log2_stride == 3
            && conf.page_table_address_space_bits > page_table_const_bits;
    } else {
        return false;
    }
}
'''

H_BLOCK = '''if (direct) {
        // nx-on-orbis 0.9.3: the decoded slot, [mirror + index * 8]: 0 is "unmapped or marked", else host - guest.
        code.mov(page, static_cast<u64>(ps4_direct_mirror_base));
        code.mov(page, qword[page + tmp.cvt64() * 8]);
        code.test(page, page);
        code.jz(abort, code.T_NEAR);
        __atomic_fetch_add(&ps4_direct_emitted, 1ull, __ATOMIC_RELAXED);
        return page + vaddr;
    }

    if (ctx.conf.page_table_log2_stride > 3) {'''


def edit_emit_h(t):
    t = once(t, r"(constexpr size_t page_table_const_mask = \(1 << page_table_const_bits\) - 1;\n)",
             lambda m: m.group(1) + H_OK, "emit_x64_memory.h constants", regex=True)
    t = once(t, "namespace Dynarmic::Backend::X64 {\n", H_DECLS + "namespace Dynarmic::Backend::X64 {\n", "emit_x64_memory.h namespace")
    start = re.search(r"EmitVAddrLookup<A64EmitContext>\(BlockOfCode& code, A64EmitContext& ctx, size_t bitsize, Xbyak::Label& abort, Xbyak::Reg64 vaddr\) \{\n", t)
    if not start:
        raise Miss("emit_x64_memory.h: EmitVAddrLookup<A64EmitContext> not found")
    end = t.find("\n}\n", start.end())
    if end < 0:
        raise Miss("emit_x64_memory.h: end of EmitVAddrLookup<A64EmitContext> not found")
    body = t[start.end(): end + 3]
    tmp_old = "const Xbyak::Reg64 tmp = ctx.conf.absolute_offset_page_table && ctx.conf.page_table_pointer_mask == 0 ? page : ctx.reg_alloc.ScratchGpr(code);"
    tmp_new = ("const bool direct = PS4DirectOk(ctx.conf);\n"
               "    const Xbyak::Reg64 tmp = ctx.conf.absolute_offset_page_table && ctx.conf.page_table_pointer_mask == 0 && !direct\n"
               "        ? page : ctx.reg_alloc.ScratchGpr(code);")
    body = once(body, tmp_old, tmp_new, "EmitVAddrLookup<A64> tmp")
    body = once(body, "if (ctx.conf.page_table_log2_stride > 3) {", H_BLOCK, "EmitVAddrLookup<A64> stride")
    return t[: start.end()] + body + t[end + 3:]


# ---------------------------------------------------------------------------------------------------------------------------
# Dynarmic: emit_x64_memory.cpp.inc (PS4ReuseSlowLookup of part 1) and a64_emit_x64_memory.cpp (EmitA64EdenPageLookup of part 1)

INC_OLD = "code.mov(page, code.qword[r14 + idx * 8]);"
INC_NEW = '''if (PS4DirectOk(conf)) {
            code.mov(page, static_cast<u64>(ps4_direct_mirror_base));
            code.mov(page, code.qword[page + idx * 8]);
            code.test(page, page);
            code.jz(abort, code.T_NEAR);
            return;
        }
        code.mov(page, code.qword[r14 + idx * 8]);'''


def edit_inc(t):
    return once(t, INC_OLD, INC_NEW, "PS4ReuseSlowLookup load")


A64_LOAD_OLD = "code.mov(host, code.qword[r14 + host * 8]);"
A64_LOAD_NEW = '''const bool direct = PS4DirectOk(conf);
    if (direct) {
        // nx-on-orbis 0.9.3: the decoded slot: 0 = unmapped or marked, else host - guest.
        code.mov(tmp, static_cast<u64>(ps4_direct_mirror_base));
        code.mov(host, code.qword[tmp + host * 8]);
        __atomic_fetch_add(&ps4_direct_emitted, 1ull, __ATOMIC_RELAXED);
    } else {
        code.mov(host, code.qword[r14 + host * 8]);
    }'''

A64_DEFS = '''

// nx-on-orbis 0.9.3: the JIT direct table (patches/eden-jit/0002-*.py). ps4_direct_present says the build has it (the frontend's weak
// 0 stands when it does not); ps4_direct_emitted counts the lookups compiled to read the decoded slot.
extern "C" {
int ps4_direct_present = 1;
unsigned long long ps4_direct_emitted = 0;
}
'''


def edit_a64(t):
    t = once(t, A64_LOAD_OLD, A64_LOAD_NEW, "EmitA64EdenPageLookup load")
    t = once(t, "if (conf.page_table_marked_bit) {\n        code.bt(host, *conf.page_table_marked_bit);",
             "if (!direct && conf.page_table_marked_bit) {\n        code.bt(host, *conf.page_table_marked_bit);", "EdenPageLookup marked bit")
    t = once(t, "if (conf.page_table_pointer_mask != 0) {\n        if (std::in_range<s32>(conf.page_table_pointer_mask)) {\n            code.and_(host,",
             "if (!direct && conf.page_table_pointer_mask != 0) {\n        if (std::in_range<s32>(conf.page_table_pointer_mask)) {\n            code.and_(host,",
             "EdenPageLookup mask")
    t = once(t, "if (conf.page_table_sign_extension) {\n        code.shl(host,", "if (!direct && conf.page_table_sign_extension) {\n        code.shl(host,",
             "EdenPageLookup sign extension")
    if not t.rstrip().endswith("}  // namespace Dynarmic::Backend::X64"):
        raise Miss("a64_emit_x64_memory.cpp does not end with the namespace close")
    return t.rstrip() + A64_DEFS


FILES = [
    ("src/common/page_table.h", edit_page_table_h),
    ("src/common/page_table.cpp", edit_page_table_cpp),
    ("src/core/memory.cpp", edit_memory_cpp),
    ("src/dynarmic/src/dynarmic/backend/x64/emit_x64_memory.h", edit_emit_h),
    ("src/dynarmic/src/dynarmic/backend/x64/emit_x64_memory.cpp.inc", edit_inc),
    ("src/dynarmic/src/dynarmic/backend/x64/a64_emit_x64_memory.cpp", edit_a64),
]


def main(argv):
    if len(argv) != 2:
        print("usage: 0002-JIT-direct-page-table-copy.py <Eden checkout>", file=sys.stderr)
        return 2
    root = pathlib.Path(argv[1])
    out = {}
    try:
        for rel, fn in FILES:
            path = root / rel
            if not path.is_file():
                raise Miss(f"{rel}: not in the checkout")
            out[path] = fn(path.read_text(encoding="utf-8", newline=""))
    except Miss as e:
        print(f"JIT direct table: does not fit this Eden ({e})", file=sys.stderr)
        return 1
    for path, text in out.items():   # all edits worked: only now is anything written
        path.write_text(text, encoding="utf-8", newline="")
    print(f"JIT direct table: {len(out)} files edited")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
