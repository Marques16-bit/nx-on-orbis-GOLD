// SPDX-License-Identifier: GPL-3.0-or-later
// Host test of src/common/orbis_lazy_memory.cpp (an Eden file that only exists as patches 0002, 0007, 0027 and
// 0029): tests/lazy_memory_test.sh rebuilds it from the patches, and this file stands in for the console's kernel
// calls with mmap on the PC, so the system flexible path (0.9.0) is run for real: the budget, the fallback to
// direct memory, the refusal switch, zeroing, decommit and release.
#include <sys/mman.h>
#include <sys/types.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

#include "common/orbis_lazy_memory.h"

namespace {
constexpr std::size_t Chunk = 64 * 1024;
constexpr int32_t MapFixedFlag = 0x10;

// ---- the console, as far as the file under test sees it ----
std::size_t g_direct_total = 64u << 20;
off_t g_next_phys = 0;
std::set<off_t> g_direct_live;        // physical chunks handed out and not released
std::set<std::uintptr_t> g_flex_live; // chunks the flexible pool holds
std::size_t g_flex_pool = 0;          // chunks the pool can hold
std::size_t g_flex_calls = 0;
bool g_flex_wrong_address = false;    // answer "ok" but map somewhere else
int g_failures = 0;

void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    g_failures += ok ? 0 : 1;
}

std::int32_t FlexMap(void** addr, std::size_t len, std::int32_t, std::int32_t flags, const char*) {
    ++g_flex_calls;
    if (len != Chunk || !(flags & MapFixedFlag)) {
        return -1;
    }
    if (g_flex_live.size() >= g_flex_pool) {
        return static_cast<std::int32_t>(0x8002000c); // ENOMEM, as the console says it
    }
    void* target = *addr;
    if (g_flex_wrong_address) {
        void* other = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        *addr = other;
        g_flex_live.insert(reinterpret_cast<std::uintptr_t>(other));
        return 0;
    }
    void* p = mmap(target, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED) {
        return -1;
    }
    std::memset(p, 0xAA, len); // the pool hands pages back with whatever was in them
    g_flex_live.insert(reinterpret_cast<std::uintptr_t>(p));
    return 0;
}
} // namespace

extern "C" {
size_t sceKernelGetDirectMemorySize(void) {
    return g_direct_total;
}
int32_t sceKernelAllocateDirectMemory(off_t, off_t end, size_t len, size_t, int32_t, off_t* out) {
    if (static_cast<std::size_t>(g_next_phys) + len > static_cast<std::size_t>(end)) {
        return -1;
    }
    *out = g_next_phys;
    g_direct_live.insert(g_next_phys);
    g_next_phys += static_cast<off_t>(len);
    return 0;
}
int32_t sceKernelMapDirectMemory(void** addr, size_t len, int32_t, int32_t, off_t, size_t) {
    // The reservation under it is PROT_NONE; "mapping" opens it up and fills it with leftovers.
    if (mprotect(*addr, len, PROT_READ | PROT_WRITE) != 0) {
        return -1;
    }
    std::memset(*addr, 0xBB, len);
    return 0;
}
int32_t sceKernelReleaseDirectMemory(off_t phys, size_t) {
    return g_direct_live.erase(phys) == 1 ? 0 : -1;
}
int32_t sceKernelReserveVirtualRange(void** addr, size_t len, int32_t flags, size_t align) {
    if (flags & MapFixedFlag) {
        void* p = mmap(*addr, len, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        return p == MAP_FAILED ? -1 : 0;
    }
    // The console aligns a reservation as asked (the code under test counts on 64 KiB): do the same.
    const std::size_t slack = align > 4096 ? align : 0;
    auto* raw = static_cast<unsigned char*>(mmap(nullptr, len + slack, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0));
    if (raw == MAP_FAILED) {
        return -1;
    }
    auto* aligned = raw;
    if (slack != 0) {
        aligned = reinterpret_cast<unsigned char*>((reinterpret_cast<std::uintptr_t>(raw) + align - 1) & ~(align - 1));
        if (aligned > raw) {
            munmap(raw, static_cast<std::size_t>(aligned - raw));
        }
        const std::size_t tail = static_cast<std::size_t>((raw + len + slack) - (aligned + len));
        if (tail != 0) {
            munmap(aligned + len, tail);
        }
    }
    *addr = aligned;
    return 0;
}
int32_t sceKernelMunmap(void* addr, size_t len) {
    for (std::size_t off = 0; off < len; off += Chunk) {
        g_flex_live.erase(reinterpret_cast<std::uintptr_t>(addr) + off);
    }
    return munmap(addr, len);
}
int sceKernelInstallExceptionHandler(int, void (*)(int, void*)) {
    return 0;
}
}

namespace {
using namespace Common::Orbis;

bool AllZero(const void* p, std::size_t n) {
    const auto* b = static_cast<const unsigned char*>(p);
    for (std::size_t i = 0; i < n; ++i) {
        if (b[i] != 0) {
            return false;
        }
    }
    return true;
}

void Reset(std::size_t pool_chunks) {
    g_flex_pool = pool_chunks;
    g_flex_calls = 0;
    g_flex_wrong_address = false;
}
} // namespace

int main() {
    // 1. Off by default: every chunk comes from direct memory.
    {
        Reset(100);
        char* base = static_cast<char*>(ReserveLazy(8 * Chunk, "t1"));
        Check(base != nullptr, "1: a range is reserved");
        Check(CommitLazy(base, 4 * Chunk), "1: four chunks commit");
        Stats s = GetStats();
        Check(s.committed_bytes == 4 * Chunk && s.system_flexible_bytes == 0, "1: all four are direct, none flexible");
        Check(g_flex_calls == 0 && g_direct_live.size() == 4, "1: the flexible pool was never asked");
        Check(AllZero(base, 4 * Chunk), "1: direct chunks come back zeroed");
        ReleaseLazy(base);
        Check(GetStats().committed_bytes == 0 && g_direct_live.empty(), "1: release gives everything back");
    }
    // 2. A budget of four chunks, six committed: four flexible, two direct.
    {
        Reset(100);
        ConfigureSystemFlexible(FlexMap, 4 * Chunk);
        char* base = static_cast<char*>(ReserveLazy(8 * Chunk, "t2"));
        Check(CommitLazy(base, 6 * Chunk), "2: six chunks commit");
        Stats s = GetStats();
        Check(s.system_flexible_bytes == 4 * Chunk, "2: four chunks sit in system flexible memory (the budget)");
        Check(s.committed_bytes == 6 * Chunk && g_direct_live.size() == 2, "2: committed counts all six, two of them direct");
        Check(AllZero(base, 6 * Chunk), "2: flexible chunks are zeroed too (the pool handed them back dirty)");
        std::memset(base, 0x5A, 6 * Chunk);
        Check(static_cast<unsigned char>(base[0]) == 0x5A && static_cast<unsigned char>(base[6 * Chunk - 1]) == 0x5A,
              "2: the memory is usable across the flexible/direct boundary");
        Check(IsCommitted(base) && IsCommitted(base + 5 * Chunk) && !IsCommitted(base + 6 * Chunk), "2: IsCommitted sees both kinds");
        // 3. Decommit two flexible chunks (and the partial ones around are skipped), then commit again: the budget is reusable.
        DecommitLazy(base, 2 * Chunk);
        s = GetStats();
        Check(s.system_flexible_bytes == 2 * Chunk && s.committed_bytes == 4 * Chunk && s.decommitted_bytes == 2 * Chunk,
              "3: decommit hands two flexible chunks back to the pool");
        Check(g_flex_live.size() == 2, "3: the pool really holds two fewer");
        Check(CommitLazy(base, 2 * Chunk), "3: the same chunks commit again");
        Check(GetStats().system_flexible_bytes == 4 * Chunk, "3: and go back to flexible memory (budget freed)");
        Check(AllZero(base, 2 * Chunk), "3: zeroed again after a decommit");
        ReleaseLazy(base);
        s = GetStats();
        Check(s.committed_bytes == 0 && s.system_flexible_bytes == 0 && g_direct_live.empty() && g_flex_live.empty(),
              "3: release empties both the direct and the flexible side");
    }
    // 4. The pool refuses: it is asked once, then direct memory takes over for good.
    {
        Reset(2);
        ConfigureSystemFlexible(FlexMap, 100 * Chunk);
        char* base = static_cast<char*>(ReserveLazy(8 * Chunk, "t4"));
        Check(CommitLazy(base, 6 * Chunk), "4: six chunks commit although the pool holds two");
        Stats s = GetStats();
        Check(s.system_flexible_bytes == 2 * Chunk && g_direct_live.size() == 4, "4: two flexible, four direct");
        Check(s.system_flexible_refusals == 1, "4: the refusal is counted once");
        Check(g_flex_calls == 3, "4: the pool is not asked again after a refusal (2 successes + 1 refusal)");
        ReleaseLazy(base);
    }
    // 5. A call that says "ok" but maps elsewhere is not trusted.
    {
        Reset(100);
        g_flex_wrong_address = true;
        const std::size_t refusals_before = GetStats().system_flexible_refusals; // the count only ever grows
        ConfigureSystemFlexible(FlexMap, 100 * Chunk);
        char* base = static_cast<char*>(ReserveLazy(4 * Chunk, "t5"));
        Check(CommitLazy(base, Chunk), "5: the chunk still commits (from direct memory)");
        Stats s = GetStats();
        Check(s.system_flexible_bytes == 0 && s.system_flexible_refusals == refusals_before + 1 && g_direct_live.size() == 1 &&
                  g_flex_live.empty(),
              "5: counted as a refusal, the stray mapping undone, direct used");
        ReleaseLazy(base);
        g_flex_wrong_address = false;
    }
    // 6. Budget 0 or a null function: off.
    {
        Reset(100);
        ConfigureSystemFlexible(FlexMap, 0);
        char* base = static_cast<char*>(ReserveLazy(2 * Chunk, "t6"));
        Check(CommitLazy(base, 2 * Chunk) && GetStats().system_flexible_bytes == 0 && g_flex_calls == 0, "6: a budget of 0 is off");
        ReleaseLazy(base);
        ConfigureSystemFlexible(nullptr, 10 * Chunk);
        base = static_cast<char*>(ReserveLazy(2 * Chunk, "t6b"));
        Check(CommitLazy(base, 2 * Chunk) && GetStats().system_flexible_bytes == 0 && g_flex_calls == 0, "6: no function is off");
        ReleaseLazy(base);
    }
    // 7. Two ranges share one budget; the offset helper (OnGuestRamFreed) reaches flexible chunks too.
    {
        Reset(100);
        ConfigureSystemFlexible(FlexMap, 3 * Chunk);
        char* a = static_cast<char*>(ReserveLazy(4 * Chunk, "t7a"));
        char* b = static_cast<char*>(ReserveLazy(4 * Chunk, "t7b"));
        SetGuestRamBase(a, 4 * Chunk);
        CommitLazy(a, 2 * Chunk);
        CommitLazy(b, 2 * Chunk);
        Check(GetStats().system_flexible_bytes == 3 * Chunk && g_direct_live.size() == 1, "7: the budget is shared by all ranges (3 flexible, 1 direct)");
        OnGuestRamFreed(0, 2 * Chunk);
        Check(GetStats().decommitted_bytes >= 2 * Chunk && GetStats().system_flexible_bytes == Chunk,
              "7: a kernel free of guest pages gives flexible chunks back");
        ReleaseLazy(a);
        ReleaseLazy(b);
        Stats s = GetStats();
        Check(s.committed_bytes == 0 && s.system_flexible_bytes == 0 && g_direct_live.empty() && g_flex_live.empty(), "7: nothing is left");
    }
    ConfigureSystemFlexible(nullptr, 0);
    std::printf("lazy memory test: %s\n", g_failures == 0 ? "PASSED" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
