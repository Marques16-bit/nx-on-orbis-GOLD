// SPDX-License-Identifier: GPL-3.0-or-later
// 0.9.7 fix: the driver's arena is placed inside one 4 GiB window (see arena_place.h for the why).
//
// The link wraps sceKernelMapDirectMemory (-Wl,--wrap=sceKernelMapDirectMemory), so the driver's call for its arena passes through
// __wrap_sceKernelMapDirectMemory. Every other call, and the arena call when the switch is off, goes to the real function unchanged.
//
// Nothing here can make the arena request fail that would not have failed anyway: the reservations are released before the real call is made
// (the chosen one) or after it (the fillers), and when anything is odd the real call is made exactly as the driver made it.
#include "arena_place.h"

#include <sys/types.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

extern "C" {
int32_t sceKernelReserveVirtualRange(void** addr, size_t len, int32_t flags, size_t alignment);
int32_t sceKernelMunmap(void* addr, size_t len);
int32_t __real_sceKernelMapDirectMemory(void** addr, size_t len, int32_t prot, int32_t flags, off_t phys, size_t alignment);
}

namespace Ps4 {
void Log(const char* fmt, ...);
}

namespace {

// 0: off, 1: armed (the next arena request is placed), 2: done (one request is placed per start).
std::atomic<int> g_state{0};
std::atomic<bool> g_placed{false};
std::atomic<std::uint64_t> g_last_final{0};

} // namespace

namespace Ps4ArenaPlace {

/// Called by main() before the game is loaded: on = place the arena, off = leave it to the kernel (the switch arena_align, safe mode).
void Arm(bool on) {
    g_state.store(on ? 1 : 0);
    g_placed.store(false);
}

/// For the log after the game has loaded: 0 = the arena request never reached the wrapper, 1 = it did and was placed (or checked).
bool Reached() {
    return g_placed.load();
}

} // namespace Ps4ArenaPlace

extern "C" int32_t __wrap_sceKernelMapDirectMemory(void** addr, size_t len, int32_t prot, int32_t flags, off_t phys, size_t alignment) {
    if (g_state.load() != 1 || addr == nullptr ||
        !Ps4ArenaPlace::IsArenaRequest(Ps4ArenaPlace::ArenaMiBFromEnv(std::getenv("ORBIS_ARENA_MIB")), len, flags, *addr == nullptr)) {
        return __real_sceKernelMapDirectMemory(addr, len, prot, flags, phys, alignment);
    }
    int expected = 1;
    if (!g_state.compare_exchange_strong(expected, 2)) { // another thread took it
        return __real_sceKernelMapDirectMemory(addr, len, prot, flags, phys, alignment);
    }
    g_placed.store(true);

    const size_t align = alignment != 0 ? alignment : (2u << 20);
    const Ps4ArenaPlace::Outcome out = Ps4ArenaPlace::Search(
        [align](std::uint64_t bytes) -> std::uint64_t {
            void* at = nullptr;
            if (sceKernelReserveVirtualRange(&at, static_cast<size_t>(bytes), 0, align) != 0 || at == nullptr) {
                return 0;
            }
            return reinterpret_cast<std::uintptr_t>(at);
        },
        len);

    const bool pad = out.chosen != 0 && out.fillers > 0;
    // The range chosen is given back just before the driver's own request, so the kernel's first fit offers it again. When no range was
    // better than the kernel's own first choice (or none could be reserved) every reservation is given back first and the call is the plain one.
    if (out.chosen != 0) {
        sceKernelMunmap(reinterpret_cast<void*>(out.chosen), static_cast<size_t>(len));
    }
    if (!pad) {
        for (unsigned i = 0; i < out.fillers; ++i) {
            sceKernelMunmap(reinterpret_cast<void*>(out.filler[i]), static_cast<size_t>(len));
        }
    }
    const int32_t rc = __real_sceKernelMapDirectMemory(addr, len, prot, flags, phys, alignment);
    if (pad) {
        for (unsigned i = 0; i < out.fillers; ++i) {
            sceKernelMunmap(reinterpret_cast<void*>(out.filler[i]), static_cast<size_t>(len));
        }
    }
    const std::uint64_t final_at = rc == 0 ? reinterpret_cast<std::uintptr_t>(*addr) : 0;
    g_last_final.store(final_at);
    Ps4::Log("%s%s", Ps4ArenaPlace::Describe(out, len, final_at).c_str(),
             rc == 0 ? "" : " (the driver's request itself then failed; that is not caused by the placement)");
    return rc;
}
