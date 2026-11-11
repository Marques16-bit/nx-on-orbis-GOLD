// SPDX-License-Identifier: GPL-3.0-or-later
// Host test of the page reuse pass in patches/eden-jit/: the pass text itself (cut out of the patch by tests/jit_pack_test.sh)
// is compiled against a small stand-in of Dynarmic's IR (ir_mock.hpp) and run on hand-built blocks. It checks which accesses are
// grouped, what the group's lookup instruction holds, and that nothing is touched when the pass must not run.
// It cannot check the machine code the emitters make (that needs Xbyak and a CPU test; see docs/TECHNICAL.md).
#include "ir_mock.hpp"
#include "small_vector_shim.hpp"

namespace Dynarmic::Optimization {
#include PASS_INC

namespace {

int g_failures = 0;
void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    g_failures += ok ? 0 : 1;
}

using IR::AccType;
using IR::Inst;
using IR::Opcode;
using IR::Value;

Inst* Push(IR::Block& b, Opcode op, Value a0 = {}, Value a1 = {}, Value a2 = {}, Value a3 = {}) {
    Inst inst;
    inst.op = op;
    inst.args = {a0, a1, a2, a3};
    b.instructions.push_back(inst);
    return &b.instructions.back();
}
Value Imm(u64 v) { return Value{v}; }
Value Loc() { return Value{u64{0x1000}}; } // the current location descriptor, an immediate in arg 0 of every A64 memory op

Inst* Load(IR::Block& b, Opcode op, Value addr, AccType t = AccType::NORMAL) { return Push(b, op, Loc(), addr, Value{t}); }
Inst* Store(IR::Block& b, Opcode op, Value addr, AccType t = AccType::NORMAL) { return Push(b, op, Loc(), addr, Imm(0), Value{t}); }

Value Plus(IR::Block& b, Value base, s64 constant) {
    return Value{Push(b, Opcode::Add64, base, Imm(static_cast<u64>(constant)), Value{false})};
}

std::size_t Count(const IR::Block& b, Opcode op) {
    return static_cast<std::size_t>(std::count_if(b.instructions.begin(), b.instructions.end(), [&](const Inst& i) { return i.op == op; }));
}
const Inst* Find(const IR::Block& b, Opcode op) {
    for (const Inst& i : b.instructions) {
        if (i.op == op) return &i;
    }
    return nullptr;
}
bool Grouped(const Inst* i) { return !i->GetArg(0).IsImmediate(); }

A64::UserConfig Config() {
    static void* table[1] = {nullptr}; // only its being non-null matters to the pass
    A64::UserConfig conf;
    conf.page_table = table;
    return conf;
}

void Reset(bool on = true) {
    ps4_page_reuse = on;
    ps4_page_reuse_status = 0;
    ps4_page_reuse_why = 0;
    ps4_page_reuse_groups = 0;
    ps4_page_reuse_accesses = 0;
}

int RunTests() {
    std::printf("-- page reuse pass\n");
    {
        Reset();
        IR::Block b;
        Inst* x = Push(b, Opcode::Other);
        Inst* a = Load(b, Opcode::A64ReadMemory64, Value{x});
        Inst* c = Load(b, Opcode::A64ReadMemory64, Plus(b, Value{x}, 8));
        Inst* d = Store(b, Opcode::A64WriteMemory64, Plus(b, Value{x}, 16));
        const bool changed = PS4PageReusePass(b, Config());
        const Inst* lookup = Find(b, Opcode::A64EdenPageLookup);
        Check(changed && lookup != nullptr && Count(b, Opcode::A64EdenPageLookup) == 1, "three accesses off one base: one lookup");
        Check(lookup != nullptr && lookup->GetArg(0).GetInst() == x && lookup->GetArg(1).GetImmediateAsU64() == 24,
              "the lookup is for the base (offset 0 needs no add) and spans 24 bytes");
        Check(Grouped(a) && Grouped(c) && Grouped(d) && a->GetArg(0).GetInst() == lookup && d->GetArg(0).GetInst() == lookup,
              "every member points at the lookup in arg 0");
        Check(&*b.instructions.begin() == x && &*std::next(b.instructions.begin()) == lookup, "the lookup sits just before the first member");
        Check(ps4_page_reuse_status == 1 && ps4_page_reuse_groups == 1 && ps4_page_reuse_accesses == 3, "status 1, one group, three accesses counted");
        Check(!PS4PageReusePass(b, Config()), "running the pass again changes nothing");
    }
    {
        Reset();
        IR::Block b;
        Inst* x = Push(b, Opcode::Other);
        Inst* a = Load(b, Opcode::A64ReadMemory32, Plus(b, Value{x}, -8));
        Inst* c = Load(b, Opcode::A64ReadMemory128, Plus(b, Value{x}, 8));
        PS4PageReusePass(b, Config());
        const Inst* lookup = Find(b, Opcode::A64EdenPageLookup);
        Check(lookup != nullptr && Grouped(a) && Grouped(c), "negative and positive offsets group");
        // low = -8, high = 8 + 16 = 24: span 32, and the lookup address is base + (-8) as an Add64 placed before the lookup
        Check(lookup != nullptr && lookup->GetArg(1).GetImmediateAsU64() == 32, "span runs from the lowest to the highest byte");
        const Inst* add = lookup != nullptr ? lookup->GetArg(0).GetInst() : nullptr;
        Check(add != nullptr && add->op == Opcode::Add64 && add->GetArg(0).GetInst() == x && static_cast<s64>(add->GetArg(1).GetImmediateAsU64()) == -8,
              "the lookup address is base + lowest offset");
    }
    {
        Reset();
        IR::Block b;
        Inst* x = Push(b, Opcode::Other);
        Value p = Plus(b, Plus(b, Value{x}, 8), 16); // (x + 8) + 16 and x + 24 are the same address
        Inst* a = Load(b, Opcode::A64ReadMemory64, p);
        Inst* c = Load(b, Opcode::A64ReadMemory64, Plus(b, Value{x}, 24));
        Inst* e = Load(b, Opcode::A64ReadMemory64, Value{Push(b, Opcode::Sub64, Value{x}, Imm(8), Value{false})});
        PS4PageReusePass(b, Config());
        Check(Grouped(a) && Grouped(c) && Grouped(e) && Count(b, Opcode::A64EdenPageLookup) == 1, "chains of constant adds and subs decompose to one root");
    }
    {
        Reset();
        IR::Block b;
        Inst* x = Push(b, Opcode::Other);
        Inst* carry = Push(b, Opcode::Add64, Value{x}, Imm(8), Value{true}); // add with carry in: not a plain offset
        Inst* a = Load(b, Opcode::A64ReadMemory64, Value{x});
        Inst* c = Load(b, Opcode::A64ReadMemory64, Value{carry});
        PS4PageReusePass(b, Config());
        Check(!Grouped(a) && !Grouped(c) && Count(b, Opcode::A64EdenPageLookup) == 0, "an add with a carry in is its own root: nothing groups");
    }
    {
        Reset();
        IR::Block b;
        Inst* x = Push(b, Opcode::Other);
        Inst* a = Load(b, Opcode::A64ReadMemory64, Value{x});
        Push(b, Opcode::A64ExclusiveReadMemory64, Loc(), Value{x}, Value{AccType::ATOMIC});
        Inst* c = Load(b, Opcode::A64ReadMemory64, Plus(b, Value{x}, 8));
        PS4PageReusePass(b, Config());
        Check(!Grouped(a) && !Grouped(c), "an exclusive access between two accesses ends the group");
    }
    for (Opcode barrier : {Opcode::A64CallSupervisor, Opcode::A64ExceptionRaised, Opcode::A64DataMemoryBarrier, Opcode::A64ClearExclusive,
                           Opcode::A64DataCacheOperationRaised, Opcode::A64InstructionSynchronizationBarrier}) {
        Reset();
        IR::Block b;
        Inst* x = Push(b, Opcode::Other);
        Inst* a = Load(b, Opcode::A64ReadMemory64, Value{x});
        Push(b, barrier);
        Inst* c = Load(b, Opcode::A64ReadMemory64, Plus(b, Value{x}, 8));
        PS4PageReusePass(b, Config());
        Check(!Grouped(a) && !Grouped(c), "a barrier, supervisor call, exception or exclusive clear ends the group");
    }
    {
        Reset();
        IR::Block b;
        Inst* x = Push(b, Opcode::Other);
        Inst* a = Load(b, Opcode::A64ReadMemory64, Value{x});
        Inst* o = Load(b, Opcode::A64ReadMemory64, Plus(b, Value{x}, 8), AccType::ORDERED);
        Inst* c = Load(b, Opcode::A64ReadMemory64, Plus(b, Value{x}, 16));
        PS4PageReusePass(b, Config());
        Check(!Grouped(a) && !Grouped(o) && !Grouped(c), "an ordered (acquire) access is never grouped and ends the group");
    }
    {
        Reset();
        IR::Block b;
        Inst* x = Push(b, Opcode::Other);
        Inst* a = Load(b, Opcode::A64ReadMemory64, Value{x}, AccType::VEC);
        Inst* c = Store(b, Opcode::A64WriteMemory128, Plus(b, Value{x}, 16), AccType::VEC);
        Inst* d = Load(b, Opcode::A64ReadMemory8, Plus(b, Value{x}, 40), AccType::UNPRIV);
        PS4PageReusePass(b, Config());
        Check(Grouped(a) && Grouped(c) && Grouped(d), "vector and unprivileged accesses group, 8 to 128 bits");
        Check(Find(b, Opcode::A64EdenPageLookup)->GetArg(1).GetImmediateAsU64() == 41, "span 41: bytes 0..40");
    }
    {
        Reset();
        IR::Block b;
        Inst* x = Push(b, Opcode::Other);
        Inst* y = Push(b, Opcode::Other);
        Inst* a = Load(b, Opcode::A64ReadMemory64, Value{x});
        Inst* c = Load(b, Opcode::A64ReadMemory64, Value{y});
        Inst* d = Load(b, Opcode::A64ReadMemory64, Plus(b, Value{x}, 8));
        Inst* e = Load(b, Opcode::A64ReadMemory64, Plus(b, Value{y}, 8));
        PS4PageReusePass(b, Config());
        Check(Grouped(a) && Grouped(d) && Grouped(c) && Grouped(e) && Count(b, Opcode::A64EdenPageLookup) == 2 &&
                  a->GetArg(0).GetInst() == d->GetArg(0).GetInst() && c->GetArg(0).GetInst() == e->GetArg(0).GetInst() &&
                  a->GetArg(0).GetInst() != c->GetArg(0).GetInst(),
              "interleaved accesses off two bases make two groups");
    }
    {
        Reset();
        IR::Block b;
        Inst* x = Push(b, Opcode::Other);
        Inst* a = Load(b, Opcode::A64ReadMemory64, Value{x});
        Inst* c = Load(b, Opcode::A64ReadMemory64, Plus(b, Value{x}, 300));
        PS4PageReusePass(b, Config());
        Check(!Grouped(a) && !Grouped(c), "accesses more than 256 bytes apart are not grouped");
    }
    {
        Reset();
        IR::Block b;
        Inst* x = Push(b, Opcode::Other);
        Inst* a = Load(b, Opcode::A64ReadMemory64, Value{x});
        Inst* c = Load(b, Opcode::A64ReadMemory64, Plus(b, Value{x}, 8));
        Inst* d = Load(b, Opcode::A64ReadMemory64, Plus(b, Value{x}, 400)); // too far: starts a new group, which stays alone
        Inst* e = Load(b, Opcode::A64ReadMemory64, Plus(b, Value{x}, 408));
        PS4PageReusePass(b, Config());
        Check(Grouped(a) && Grouped(c) && Grouped(d) && Grouped(e) && Count(b, Opcode::A64EdenPageLookup) == 2,
              "a far access closes the first group and opens a second");
    }
    {
        Reset();
        IR::Block b;
        Inst* x = Push(b, Opcode::Other);
        Inst* a = Load(b, Opcode::A64ReadMemory64, Value{x});
        PS4PageReusePass(b, Config());
        Check(!Grouped(a) && Count(b, Opcode::A64EdenPageLookup) == 0, "one access alone is left as it is");
    }
    {
        Reset();
        IR::Block b;
        Inst* a = Load(b, Opcode::A64ReadMemory64, Imm(0x4000));
        Inst* c = Load(b, Opcode::A64ReadMemory64, Imm(0x4008));
        PS4PageReusePass(b, Config());
        Check(!Grouped(a) && !Grouped(c), "constant addresses have no root and are left alone");
    }
    std::printf("-- the pass must not run\n");
    {
        Reset(false);
        IR::Block b;
        Inst* x = Push(b, Opcode::Other);
        Load(b, Opcode::A64ReadMemory64, Value{x});
        Load(b, Opcode::A64ReadMemory64, Plus(b, Value{x}, 8));
        const std::size_t before = b.instructions.size();
        Check(!PS4PageReusePass(b, Config()) && b.instructions.size() == before && ps4_page_reuse_status == 2, "switch off: nothing changes, status 2");
    }
    struct Bad {
        const char* what;
        void (*set)(A64::UserConfig&);
        unsigned bit;
    };
    const Bad bads[] = {
        {"no page table", [](A64::UserConfig& c) { c.page_table = nullptr; }, 0},
        {"fastmem on", [](A64::UserConfig& c) { c.fastmem_pointer = 0x1000; }, 1},
        {"halt checks on memory access", [](A64::UserConfig& c) { c.check_halt_on_memory_access = true; }, 2},
        {"relative offsets", [](A64::UserConfig& c) { c.absolute_offset_page_table = false; }, 3},
        {"mirrored address space", [](A64::UserConfig& c) { c.silently_mirror_page_table = true; }, 4},
        {"16-byte entries", [](A64::UserConfig& c) { c.page_table_log2_stride = 4; }, 5},
        {"address space over 42 bits", [](A64::UserConfig& c) { c.page_table_address_space_bits = 48; }, 6},
        {"address space of 12 bits", [](A64::UserConfig& c) { c.page_table_address_space_bits = 12; }, 6},
        {"misalignment checked on every access", [](A64::UserConfig& c) { c.only_detect_misalignment_via_page_table_on_page_boundary = false; }, 7},
    };
    static int dummy;
    for (const Bad& bad : bads) {
        Reset();
        IR::Block b;
        Inst* x = Push(b, Opcode::Other);
        Load(b, Opcode::A64ReadMemory64, Value{x});
        Load(b, Opcode::A64ReadMemory64, Plus(b, Value{x}, 8));
        const std::size_t before = b.instructions.size();
        A64::UserConfig conf = Config();
        void* table[1] = {&dummy};
        conf.page_table = table;
        bad.set(conf);
        const bool changed = PS4PageReusePass(b, conf);
        char what[160];
        std::snprintf(what, sizeof what, "%s: nothing changes, status 3, reason bit %u", bad.what, bad.bit);
        Check(!changed && b.instructions.size() == before && ps4_page_reuse_status == 3 && (ps4_page_reuse_why & (1u << bad.bit)) != 0, what);
    }
    {
        // Eden's configuration: the marked bit is 0, the sign extension 8 (patch 0007), the mask not a small number.
        Reset();
        IR::Block b;
        void* table[1] = {&g_failures};
        A64::UserConfig conf = Config();
        conf.page_table = table;
        Inst* x = Push(b, Opcode::Other);
        Load(b, Opcode::A64ReadMemory64, Value{x});
        Load(b, Opcode::A64ReadMemory64, Plus(b, Value{x}, 8));
        Check(PS4PageReuseConfigWhy(conf) == 0 && PS4PageReusePass(b, conf), "Eden's table configuration is accepted");
        conf.page_table_marked_bit.reset();
        conf.page_table_sign_extension.reset();
        Check(PS4PageReuseConfigWhy(conf) == 0, "a table without a marked bit or sign extension is accepted too (the emitters skip those steps)");
    }
    std::printf("%s\n", g_failures == 0 ? "all ok" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}

} // namespace
} // namespace Dynarmic::Optimization

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    return Dynarmic::Optimization::RunTests();
}
