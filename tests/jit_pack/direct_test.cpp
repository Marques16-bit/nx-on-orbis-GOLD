// Host test of the page table half of the JIT direct table (patches/eden-jit/0002-*.py): common/page_table.h and page_table.cpp as that script
// leaves them, compiled with __ORBIS__ against tests/jit_pack/direct_shim (the shim reproduces the console's lazy vector). The invariant under
// test: a direct slot is 0 or exactly the entry's pointer (host - guest), whatever sequence of Store / MarkRasterizerCached / zeroing / threads
// touched the entry. Run by tests/jit_pack_test.sh.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>
#include <vector>

#include "common/page_table.h"

// What frontend/jit_pack.h defines on the console.
extern "C" {
bool ps4_direct_table = false;
bool ps4_direct_live = false;
std::uintptr_t ps4_direct_entries_base = 0;
std::uintptr_t ps4_direct_entries_bytes = 0;
std::uintptr_t ps4_direct_mirror_base = 0;
}

using Common::PageTable;
using Common::PageType;

static int g_fail = 0, g_checks = 0;
#define CHECK(cond)                                                                                  \
    do {                                                                                             \
        ++g_checks;                                                                                  \
        if (!(cond)) {                                                                               \
            ++g_fail;                                                                                \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                              \
        }                                                                                            \
    } while (0)

static std::atomic<u64>* Slot(std::size_t i) { return reinterpret_cast<std::atomic<u64>*>(ps4_direct_mirror_base) + i; }
static u64 SlotValue(std::size_t i) { return Slot(i)->load(); }

// Mirrors what the JIT would decide: slot == the entry's pointer when the entry is not marked, else 0.
static bool Consistent(PageTable& pt, std::size_t i) {
    return SlotValue(i) == static_cast<u64>(pt.entries.data()[i].Pointer());
}

int main() {
    constexpr std::size_t kBits = 32, kPageBits = 12, kEntries = std::size_t{1} << (kBits - kPageBits);
    constexpr std::size_t kPerPage = Common::HostPageSize / 8;

    // Without the option nothing exists and Store works as before.
    {
        PageTable off;
        off.Resize(kBits, kPageBits);
        CHECK(!ps4_direct_live && !off.direct_live && ps4_direct_mirror_base == 0);
        off.entries.CommitRegion(0, 8);
        auto& e = const_cast<Common::PageTable::PageEntryData&>(off.entries.data()[3]);
        e.Store(false, PageType::Memory, 7, 0x7f0000001000ull);
        CHECK(e.Pointer() == 0x7f0000001000ull && e.Block() == 7);
        off.ZeroDirect(0, kEntries); // no-op
    }

    ps4_direct_table = true;
    PageTable pt;
    pt.Resize(kBits, kPageBits);
    CHECK(ps4_direct_live && pt.direct_live);
    CHECK(ps4_direct_entries_base == reinterpret_cast<std::uintptr_t>(pt.entries.data()));
    CHECK(ps4_direct_entries_bytes == kEntries * 8);
    CHECK(ps4_direct_mirror_base != 0 && ps4_direct_mirror_base != ps4_direct_entries_base);
    pt.Resize(kBits, kPageBits); // same size again: same table, same mirror
    CHECK(ps4_direct_live && pt.direct_live && ps4_direct_mirror_base != 0);

    auto E = [&](std::size_t i) -> Common::PageTable::PageEntryData& {
        return const_cast<Common::PageTable::PageEntryData&>(pt.entries.data()[i]);
    };
    pt.entries.CommitRegion(0, 4 * kPerPage);

    // The basic transitions.
    const std::uintptr_t host = 0x7f1234560000ull;
    E(5).Store(false, PageType::Memory, 3, host);
    CHECK(SlotValue(5) == host && Consistent(pt, 5));
    E(5).MarkRasterizerCached();
    CHECK(SlotValue(5) == 0 && E(5).Type() == PageType::RasterizerCachedMemory && Consistent(pt, 5));
    E(5).Store(false, PageType::Memory, 3, host);
    CHECK(SlotValue(5) == host);
    E(5).MarkDebug(host, 3);
    CHECK(SlotValue(5) == 0 && E(5).Type() == PageType::DebugMemory);
    E(5).Store(true, PageType::Memory, 3, host); // marked: slow even though the entry has a pointer
    CHECK(SlotValue(5) == 0 && E(5).Pointer(true) == host);
    E(5).Store(false, PageType::Unmapped, 0, 0);
    CHECK(SlotValue(5) == 0);
    // A negative host - guest delta (the packed entry sign-extends it): the slot holds the same sign-extended value the decoding gives.
    const std::uintptr_t neg = static_cast<std::uintptr_t>(-0x40000000ll);
    E(6).Store(false, PageType::Memory, 1, neg);
    CHECK(SlotValue(6) == static_cast<u64>(neg) && Consistent(pt, 6));
    // The "unmapped / empty" state of a fresh entry.
    CHECK(SlotValue(7) == 0);

    // Random single-thread sequence over a few hundred entries: the invariant after every operation.
    {
        std::mt19937_64 rng(12345);
        const std::uintptr_t deltas[] = {0x7f0000000000ull, 0x7f1234560000ull, static_cast<std::uintptr_t>(-0x20000000ll), 0x100000ull};
        bool all = true;
        for (int n = 0; n < 200000; ++n) {
            const std::size_t i = rng() % 300;
            switch (rng() % 6) {
            case 0: E(i).Store(false, PageType::Memory, static_cast<u16>(rng() % 500), deltas[rng() % 4]); break;
            case 1: E(i).Store(true, PageType::DebugMemory, static_cast<u16>(rng() % 500), deltas[rng() % 4]); break;
            case 2: E(i).MarkRasterizerCached(); break;
            case 3: E(i).Store(false, PageType::Unmapped, 0, 0); break;
            case 4: E(i).MarkDebug(deltas[rng() % 4], static_cast<u16>(rng() % 500)); break;
            default: E(i).Store(false, PageType::Memory, 9, deltas[rng() % 4]); break;
            }
            all = all && Consistent(pt, i);
        }
        CHECK(all);
        bool every = true;
        for (std::size_t i = 0; i < 300; ++i) {
            every = every && Consistent(pt, i);
        }
        CHECK(every);
    }

    // Zeroing: only entries pages that have memory are touched; the ones that have none keep whatever the mirror holds there
    // (it must not commit memory for them), and a second page table is not touched at all.
    {
        for (std::size_t i = 0; i < 4 * kPerPage; i += 97) {
            E(i).Store(false, PageType::Memory, 1, host);
        }
        const std::size_t cold = 10 * kPerPage + 3; // its entries page was never committed
        Slot(cold)->store(0xAAAA);
        pt.ZeroDirect(0, 12 * kPerPage);
        bool zero = true;
        for (std::size_t i = 0; i < 4 * kPerPage; ++i) {
            zero = zero && SlotValue(i) == 0;
        }
        CHECK(zero);
        CHECK(SlotValue(cold) == 0xAAAA);
        // A range that starts and ends in the middle of pages.
        for (std::size_t i = 0; i < 4 * kPerPage; i += 1) {
            if (i % 2 == 0) {
                E(i).Store(false, PageType::Memory, 1, host);
            }
        }
        pt.ZeroDirect(kPerPage + 5, 3 * kPerPage + 9);
        CHECK(SlotValue(kPerPage + 4) == host && SlotValue(kPerPage + 5) == 0);
        CHECK(SlotValue(3 * kPerPage + 8) == 0 && SlotValue(3 * kPerPage + 10) == host);
        // Stores after the zeroing work again.
        E(kPerPage + 6).Store(false, PageType::Memory, 1, host);
        CHECK(SlotValue(kPerPage + 6) == host);
    }

    // A second table while the first is mirrored: no mirror of its own, and its stores do not reach the first one's slots.
    {
        PageTable two;
        two.Resize(kBits, kPageBits);
        CHECK(!two.direct_live && ps4_direct_live && ps4_direct_entries_base == reinterpret_cast<std::uintptr_t>(pt.entries.data()));
        two.entries.CommitRegion(0, 8);
        const u64 before = SlotValue(1);
        const_cast<Common::PageTable::PageEntryData&>(two.entries.data()[1]).Store(false, PageType::Memory, 1, host);
        CHECK(SlotValue(1) == before);
        two.ZeroDirect(0, 100); // no mirror: nothing
        CHECK(SlotValue(1) == before);
    }
    CHECK(ps4_direct_live && pt.direct_live); // the second table's death left the first one alone

    // Threads storing and marking the same few entries: at the end every slot is 0 or the entry's pointer.
    {
        std::atomic<bool> go{false};
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&, t] {
                std::mt19937_64 rng(1000 + t);
                while (!go.load()) {
                }
                for (int n = 0; n < 300000; ++n) {
                    const std::size_t i = 1000 + rng() % 4;
                    switch (rng() % 4) {
                    case 0: E(i).Store(false, PageType::Memory, static_cast<u16>(t), host + 0x1000ull * (rng() % 8)); break;
                    case 1: E(i).MarkRasterizerCached(); break;
                    case 2: E(i).Store(false, PageType::Memory, static_cast<u16>(t), host); break;
                    default: E(i).Store(true, PageType::DebugMemory, static_cast<u16>(t), host); break;
                    }
                }
            });
        }
        std::atomic<bool> reader_ok{true};
        std::thread reader([&] { // what a JIT thread does: read slot, and a non-zero slot is always some page's pointer
            while (!go.load()) {
            }
            for (int n = 0; n < 2000000; ++n) {
                const u64 v = SlotValue(1000 + n % 4);
                if (v != 0 && (v < host || v >= host + 8 * 0x1000ull)) {
                    reader_ok = false;
                }
            }
        });
        go = true;
        for (auto& th : threads) {
            th.join();
        }
        reader.join();
        CHECK(reader_ok.load());
        bool end = true;
        for (std::size_t i = 1000; i < 1004; ++i) {
            end = end && Consistent(pt, i);
        }
        CHECK(end);
    }

    // The table going away takes the globals with it.
    {
        auto* heap = new PageTable;
        ps4_direct_table = true;
        // Not the first table: no mirror, so its death must leave the first one's globals alone.
        heap->Resize(kBits, kPageBits);
        CHECK(!heap->direct_live);
        delete heap;
        CHECK(ps4_direct_live);
    }
    const std::uintptr_t keep_entries = ps4_direct_entries_base;
    (void)keep_entries;
    {
        // Rebuild in a scope of its own to see the globals reset.
        ps4_direct_live = false; // as a fresh process
        ps4_direct_entries_base = 0;
        ps4_direct_entries_bytes = 0;
        ps4_direct_mirror_base = 0;
        PageTable* fresh = new PageTable;
        fresh->Resize(kBits, kPageBits);
        CHECK(ps4_direct_live && fresh->direct_live);
        delete fresh;
        CHECK(!ps4_direct_live && ps4_direct_entries_base == 0 && ps4_direct_entries_bytes == 0 && ps4_direct_mirror_base == 0);
    }

    std::printf("direct table: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
