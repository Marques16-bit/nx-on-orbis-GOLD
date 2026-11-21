// SPDX-License-Identifier: GPL-3.0-or-later
// 0.9.7 fix: who asks the kernel for the big blocks of the C heap.
//
// musl's malloc gets all its memory through one call, __mmap (orbis-compat answers it from the direct-memory carve-outs): a block of its own for every
// allocation over 224 KiB, and the steps by which the heap grows. The Mario Maker 2 test died asking for carve-outs of 128, 128, 256, 256 and 512 MiB within
// 0.6 s, and nothing in boot.log said who asked. The link wraps __mmap (-Wl,--wrap=__mmap); every request of 16 MiB or more is written to a small ring (its
// size and the return addresses above it) and the request goes on UNCHANGED. Nothing is logged at that moment: the call can be inside malloc, holding its
// lock, and a log call that allocated would wait for that lock for ever. Ps4HeapBigMaps() prints the ring (every 30 s with the heap census, and when the process dies).
//
// A file of its own (not part of heap_census.cpp) because the host test of the census links that file without this wrapper.
#include <atomic>
#include <cstddef>
#include <cstdint>

#include <sys/types.h>

extern "C" void* __real___mmap(void* addr, size_t len, int prot, int flags, int fd, off_t off);

namespace Ps4 {
void Log(const char* fmt, ...);
}

namespace {

constexpr std::uintptr_t ImageBase = 0x400000; // as the census and the thread dump print addresses: eboot+offset
constexpr size_t BigThreshold = 16u << 20;
constexpr int BigRing = 32;

struct BigMap {
    size_t len;
    std::uint64_t site[4];
};
BigMap g_big[BigRing];
std::atomic<std::uint64_t> g_big_seq{0};
std::atomic<std::uint64_t> g_big_bytes{0};

/// The return addresses above the wrapper, from the frame pointers (the build keeps them): out[0] is where musl's malloc (or its heap growth) called the wrapper,
/// then the callers above it. Frames: BigSites <- RecordBig <- __wrap___mmap <- musl <- ..., so the first two returns are ours and are dropped.
__attribute__((noinline)) void BigSites(std::uint64_t out[4]) {
    auto* rbp = static_cast<std::uint64_t*>(__builtin_frame_address(0));
    std::uint64_t ret[6] = {};
    for (int i = 0; i < 6 && rbp != nullptr; ++i) {
        ret[i] = rbp[1];
        auto* next = reinterpret_cast<std::uint64_t*>(rbp[0]);
        if (next <= rbp || reinterpret_cast<std::uintptr_t>(next) - reinterpret_cast<std::uintptr_t>(rbp) > (1u << 20)) {
            break;
        }
        rbp = next;
    }
    for (int i = 0; i < 4; ++i) {
        out[i] = ret[i + 2];
    }
}

__attribute__((noinline)) void RecordBig(size_t len) {
    std::uint64_t site[4] = {};
    BigSites(site);
    const std::uint64_t n = g_big_seq.fetch_add(1);
    BigMap& slot = g_big[n % BigRing];
    slot.len = len;
    for (int i = 0; i < 4; ++i) {
        slot.site[i] = site[i];
    }
    g_big_bytes.fetch_add(len);
}

} // namespace

extern "C" {

void* __wrap___mmap(void* addr, size_t len, int prot, int flags, int fd, off_t off) {
    if (len >= BigThreshold) {
        RecordBig(len);
    }
    return __real___mmap(addr, len, prot, flags, fd, off);
}

/// The last requests of 16 MiB or more that musl's malloc made of the kernel, newest first.
void Ps4HeapBigMaps() {
    const std::uint64_t seen = g_big_seq.load();
    if (seen == 0) {
        Ps4::Log("heap big maps: none of 16 MiB or more asked of the heap's mapping call so far");
        return;
    }
    Ps4::Log("heap big maps: %lu request(s) of 16 MiB or more so far, %lu MiB in all; the last %d, newest first (the address musl's malloc called from, then the callers above it):",
             static_cast<unsigned long>(seen), static_cast<unsigned long>(g_big_bytes.load() >> 20),
             seen < static_cast<std::uint64_t>(BigRing) ? static_cast<int>(seen) : BigRing);
    for (std::uint64_t n = 0; n < seen && n < static_cast<std::uint64_t>(BigRing) && n < 12; ++n) {
        const BigMap& m = g_big[(seen - 1 - n) % BigRing];
        Ps4::Log("   %6lu MiB  eboot+0x%lx <- 0x%lx <- 0x%lx <- 0x%lx", static_cast<unsigned long>(m.len >> 20),
                 static_cast<unsigned long>(m.site[0] - ImageBase), static_cast<unsigned long>(m.site[1] - ImageBase),
                 static_cast<unsigned long>(m.site[2] - ImageBase), static_cast<unsigned long>(m.site[3] - ImageBase));
    }
}

} // extern "C"
