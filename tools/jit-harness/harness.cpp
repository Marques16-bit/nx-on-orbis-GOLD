// JIT harness: runs random AArch64 memory-access programs through Dynarmic's x64 backend (the pinned Eden's, with the nx-on-orbis JIT pack) with a
// page table laid out like Eden's, in four configurations, and compares everything observable: registers, vector registers, guest memory, and the
// exact sequence of memory callbacks. usage: harness <base|reuse|direct|both> <tests.bin> <count> [epochs]
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <vector>
#include <sys/mman.h>

#include "common/page_table.h"
#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/interface/exclusive_monitor.h"

extern "C" {
bool ps4_direct_table = false;
bool ps4_direct_live = false;
std::uintptr_t ps4_direct_entries_base = 0;
std::uintptr_t ps4_direct_entries_bytes = 0;
std::uintptr_t ps4_direct_mirror_base = 0;
extern bool ps4_page_reuse;
extern int ps4_page_reuse_present;
extern int ps4_page_reuse_status;
extern unsigned ps4_page_reuse_why;
extern unsigned long long ps4_page_reuse_groups;
extern unsigned long long ps4_page_reuse_accesses;
extern int ps4_direct_present;
extern unsigned long long ps4_direct_emitted;
}

using namespace Dynarmic;
static constexpr std::uint64_t kGuestBase = 0x6000000000ull;   // data, in a 39-bit space; the host buffer is far below: host - guest is negative
static constexpr std::size_t kPages = 64;
static constexpr std::uint64_t kCodeBase = 0x10000000ull;
static constexpr std::size_t kCodeSlot = 4096;

struct Env final : A64::UserCallbacks {
    std::uint8_t* host = nullptr;               // kPages * 4096 bytes behind kGuestBase
    std::vector<std::uint8_t> code;
    A64::Jit* jit = nullptr;
    std::uint64_t trace_hash = 1469598103934665603ull;
    std::uint64_t cb_count = 0;
    std::uint64_t exceptions = 0;
    void Note(char kind, int size, std::uint64_t addr) {
        ++cb_count;
        for (std::uint64_t v : {std::uint64_t(kind), std::uint64_t(size), addr}) {
            trace_hash = (trace_hash ^ v) * 1099511628211ull;
        }
    }
    bool Mapped(std::uint64_t a) const { return a >= kGuestBase && a < kGuestBase + kPages * 4096; }
    std::uint8_t Rd(std::uint64_t a) const { return Mapped(a) ? host[a - kGuestBase] : std::uint8_t((a * 31 + 7) & 0xff); }
    void Wr(std::uint64_t a, std::uint8_t v) { if (Mapped(a)) host[a - kGuestBase] = v; }
    template <typename T> T Read(std::uint64_t a) const { T v = 0; for (size_t i = 0; i < sizeof(T); ++i) v |= T(Rd(a + i)) << (8 * i); return v; }
    template <typename T> void Write(std::uint64_t a, T v) { for (size_t i = 0; i < sizeof(T); ++i) Wr(a + i, std::uint8_t(v >> (8 * i))); }

    std::optional<std::uint32_t> MemoryReadCode(std::uint64_t va) override {
        if (va < kCodeBase || va + 4 > kCodeBase + code.size()) return std::nullopt;
        std::uint32_t v; std::memcpy(&v, &code[va - kCodeBase], 4); return v;
    }
    std::uint8_t MemoryRead8(std::uint64_t a) override { Note('r', 1, a); return Read<std::uint8_t>(a); }
    std::uint16_t MemoryRead16(std::uint64_t a) override { Note('r', 2, a); return Read<std::uint16_t>(a); }
    std::uint32_t MemoryRead32(std::uint64_t a) override { Note('r', 4, a); return Read<std::uint32_t>(a); }
    std::uint64_t MemoryRead64(std::uint64_t a) override { Note('r', 8, a); return Read<std::uint64_t>(a); }
    A64::Vector MemoryRead128(std::uint64_t a) override { Note('r', 16, a); return {Read<std::uint64_t>(a), Read<std::uint64_t>(a + 8)}; }
    void MemoryWrite8(std::uint64_t a, std::uint8_t v) override { Note('w', 1, a); Write(a, v); }
    void MemoryWrite16(std::uint64_t a, std::uint16_t v) override { Note('w', 2, a); Write(a, v); }
    void MemoryWrite32(std::uint64_t a, std::uint32_t v) override { Note('w', 4, a); Write(a, v); }
    void MemoryWrite64(std::uint64_t a, std::uint64_t v) override { Note('w', 8, a); Write(a, v); }
    void MemoryWrite128(std::uint64_t a, A64::Vector v) override { Note('w', 16, a); Write(a, v[0]); Write(a + 8, v[1]); }
    bool MemoryWriteExclusive8(std::uint64_t a, std::uint8_t v, std::uint8_t e) override { Note('x', 1, a); if (Read<std::uint8_t>(a) != e) return false; Write(a, v); return true; }
    bool MemoryWriteExclusive16(std::uint64_t a, std::uint16_t v, std::uint16_t e) override { Note('x', 2, a); if (Read<std::uint16_t>(a) != e) return false; Write(a, v); return true; }
    bool MemoryWriteExclusive32(std::uint64_t a, std::uint32_t v, std::uint32_t e) override { Note('x', 4, a); if (Read<std::uint32_t>(a) != e) return false; Write(a, v); return true; }
    bool MemoryWriteExclusive64(std::uint64_t a, std::uint64_t v, std::uint64_t e) override { Note('x', 8, a); if (Read<std::uint64_t>(a) != e) return false; Write(a, v); return true; }
    bool MemoryWriteExclusive128(std::uint64_t a, A64::Vector v, A64::Vector e) override { Note('x', 16, a); if (Read<std::uint64_t>(a) != e[0] || Read<std::uint64_t>(a + 8) != e[1]) return false; Write(a, v[0]); Write(a + 8, v[1]); return true; }
    void CallSVC(std::uint32_t) override { jit->HaltExecution(); }
    void ExceptionRaised(std::uint64_t, A64::Exception) override { ++exceptions; jit->HaltExecution(); }
    void AddTicks(std::uint64_t) override {}
    std::uint64_t GetTicksRemaining() override { return 1ull << 40; }
    std::uint64_t GetCNTPCT() override { return 0; }
};

static std::uint64_t Fnv(const void* p, size_t n, std::uint64_t h = 1469598103934665603ull) {
    auto* b = static_cast<const std::uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: harness <base|reuse|direct|both> <tests.bin> <count> [epochs] [seed]\n"); return 2; }
    const std::string mode = argv[1];
    const int count = std::atoi(argv[3]);
    const int epochs = argc > 4 ? std::atoi(argv[4]) : 3;
    const unsigned seed = argc > 5 ? unsigned(std::atoi(argv[5])) : 1;
    const bool want_reuse = mode == "reuse" || mode == "both";
    const bool want_direct = mode == "direct" || mode == "both";
    ps4_page_reuse = want_reuse;
    ps4_direct_table = want_direct;

    Env env;
    {
        std::ifstream f(argv[2], std::ios::binary);
        env.code.assign(std::istreambuf_iterator<char>(f), {});
    }
    // host memory low in the address space: host - guest is negative and has to be sign-extended by the JIT
    env.host = static_cast<std::uint8_t*>(mmap(reinterpret_cast<void*>(0x20000000ull), kPages * 4096, PROT_READ | PROT_WRITE,
                                               MAP_PRIVATE | MAP_ANON | MAP_FIXED_NOREPLACE, -1, 0));
    if (env.host == MAP_FAILED) { std::perror("mmap"); return 3; }
    std::mt19937_64 mem_rng(seed * 7919u);

    Common::PageTable pt;
    pt.Resize(39, 12);
    std::vector<int> state(kPages, 0);   // 0 fast, 1 rasterizer-cached, 2 debug, 3 unmapped
    auto map_page = [&](size_t p, int st) {
        pt.entries.CommitRegion(((kGuestBase >> 12) + p), ((kGuestBase >> 12) + p + 1));
        auto& e = const_cast<Common::PageTable::PageEntryData&>(pt.entries.data()[(kGuestBase >> 12) + p]);
        const std::uintptr_t delta = reinterpret_cast<std::uintptr_t>(env.host + p * 4096) - (kGuestBase + p * 4096);
        switch (st) {
        case 0: e.Store(false, Common::PageType::Memory, 1, delta); break;
        case 1: e.Store(false, Common::PageType::Memory, 1, delta); e.MarkRasterizerCached(); break;
        case 2: e.MarkDebug(delta, 1); break;
        default: e.Store(false, Common::PageType::Unmapped, 0, 0); break;
        }
        state[p] = st;
    };
    auto randomize_states = [&]() {
        for (size_t p = 0; p < kPages; ++p) {
            const unsigned r = unsigned(mem_rng() % 100);
            map_page(p, r < 70 ? 0 : r < 85 ? 1 : r < 93 ? 2 : 3);
        }
    };

    A64::UserConfig config;
    config.callbacks = &env;
    config.page_table = reinterpret_cast<void**>(const_cast<Common::PageTable::PageEntryData*>(pt.entries.data()));
    config.page_table_address_space_bits = 39;
    config.page_table_pointer_mask = Common::PageTable::ATTRIBUTE_MASK;
    config.page_table_marked_bit = 0;
    config.silently_mirror_page_table = false;
    config.absolute_offset_page_table = true;
    config.detect_misaligned_access_via_page_table = 16 | 32 | 64 | 128;
    config.only_detect_misalignment_via_page_table_on_page_boundary = true;
    config.page_table_sign_extension = std::uint8_t(std::countl_zero(Common::PageTable::ATTRIBUTE_MASK));
    config.define_unpredictable_behaviour = true;
    config.wall_clock_cntpct = true;
    config.enable_cycle_counting = false;
    config.dczid_el0 = 4;
    config.ctr_el0 = 0x8444c004;
    config.cntfrq_el0 = 19200000;
    config.global_monitor = nullptr;
    ExclusiveMonitor monitor(1);
    config.global_monitor = &monitor;
    A64::Jit jit(config);
    env.jit = &jit;

    std::printf("mode %s: page reuse in build %d switch %d; direct table in build %d, live %d\n", mode.c_str(), ps4_page_reuse_present, int(ps4_page_reuse),
                ps4_direct_present, int(ps4_direct_live));

    std::mt19937_64 rng(seed);
    int mismatches = 0;
    for (int e = 0; e < epochs; ++e) {
        // memory contents and page states for this epoch (the same in every mode: the generators are seeded alike)
        for (size_t i = 0; i < kPages * 4096; ++i) env.host[i] = std::uint8_t(mem_rng());
        randomize_states();
        if (std::getenv("PROBE_ZERO") && e >= 1 && ps4_direct_mirror_base) for (size_t p = 0; p < kPages; ++p) reinterpret_cast<std::uint64_t*>(ps4_direct_mirror_base)[(kGuestBase >> 12) + p] = 0; // PROBE_ZERO
        for (int t = 0; t < count; ++t) {
            std::array<std::uint64_t, 31> regs;
            for (auto& r : regs) r = rng();
            const std::uint64_t page_a = kGuestBase + 4096ull * (rng() % (kPages - 8) + 2);
            const std::uint64_t pos[3] = {rng() % 4096, 4096 - 8 * (1 + rng() % 4), 16 * (rng() % 256)}; // anywhere, or hugging the page end
            regs[1] = page_a + pos[rng() % 3];
            regs[2] = kGuestBase + 4096ull * (rng() % (kPages - 8) + 2) + pos[rng() % 3];
            regs[3] = kGuestBase + 4096ull * (rng() % (kPages - 8) + 2) + pos[rng() % 3];
            regs[4] = 8 * (rng() % 64);
            regs[5] = (kGuestBase + 4096ull * (rng() % (kPages - 8) + 2) + 16 * (rng() % 128)) & ~15ull;
            const std::uint64_t sp = kGuestBase + 4096ull * 20 + 16 * (rng() % 1024);
            for (size_t i = 0; i < 31; ++i) jit.SetRegister(i, regs[i]);
            jit.SetSP(sp);
            for (int v = 0; v < 32; ++v) jit.SetVector(v, {rng(), rng()});
            jit.SetPstate(0);
            jit.SetPC(kCodeBase + kCodeSlot * t);
            env.trace_hash = 1469598103934665603ull;
            env.cb_count = 0;
            env.exceptions = 0;
            jit.Run();
            std::uint64_t h = Fnv(env.host, kPages * 4096);
            const auto r = jit.GetRegisters();
            h = Fnv(r.data(), sizeof r, h);
            const std::uint64_t spv = jit.GetSP();
            h = Fnv(&spv, 8, h);
            const auto vv = jit.GetVectors();
            h = Fnv(vv.data(), sizeof vv, h);
            std::printf("e%d t%d regs+mem %016llx trace %016llx callbacks %llu exc %llu\n", e, t, (unsigned long long)h, (unsigned long long)env.trace_hash,
                        (unsigned long long)env.cb_count, (unsigned long long)env.exceptions);
        }
    }
    std::printf("groups %llu accesses %llu direct_emitted %llu\n", ps4_page_reuse_groups, ps4_page_reuse_accesses, ps4_direct_emitted);
    std::printf("status reuse %d why %u\n", ps4_page_reuse_status, ps4_page_reuse_why);
    return mismatches;
}
