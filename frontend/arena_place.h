// SPDX-License-Identifier: GPL-3.0-or-later
// 0.9.7 fix: where the driver's memory arena sits in the address space.
//
// The Mesa driver takes ONE arena of direct memory (ORBIS_ARENA_MIB, 1280 MiB on a roomy console) and the kernel picks its address. RADEON_FLAG_32BIT
// allocations (shader binaries, descriptor tables, anything the GPU reaches through a 32-bit pointer) must lie in the 4 GiB window of the arena's start
// (address32_hi). When the arena crosses a 4 GiB boundary, only the part below the boundary can serve them.
//
// The Super Mario Odyssey test of 0.9.7 died of exactly that: arena 0x3d5c00000..0x435c00000, RADV's window 0x3d7c04000.., boundary at 0x400000000, so
// 643 MiB of the 1503 MiB could serve 32-bit allocations; at 160 s the driver said "out of 32-bit VA for 16384 bytes", Vulkan returned
// VK_ERROR_OUT_OF_HOST_MEMORY and the game stopped. Where the kernel puts the arena moves from run to run (it follows what was mapped before), so
// the same game passes in one run and dies in the next.
//
// The fix is to choose the address: when the driver asks for its arena, ask the kernel for address space of the same size first (reservations
// only, no memory), and while the offered range straddles a boundary keep it reserved and ask again - the kernel's first-fit then offers the next
// hole. The first range that lies in one window is released and the driver's own request (unchanged) takes it; the others are released after.
// The decision is pure arithmetic and lives here, so the PC tests run it against a model of the kernel's address space (tests/memory_guard_test.cpp).
// The kernel calls are in arena_place.cpp (an interception of sceKernelMapDirectMemory, linked with --wrap).
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace Ps4ArenaPlace {

constexpr std::uint64_t WindowBytes = 1ull << 32; // what a 32-bit pointer reaches
/// The driver keeps the start of its arena for itself (fence labels and fence command buffers: 32 MiB + 16 KiB); RADV's window starts after them.
/// mesa.log: "arena 1536 MiB, RADV's window 0x3d7c04000..0x435c00000" for an arena at 0x3d5c00000.
constexpr std::uint64_t PrivateBytes = 0x2004000;
constexpr unsigned MaxTries = 6; // reservations asked for before giving up (each only reserves address space, none takes memory)
constexpr std::uint64_t MinArenaMiB = 256; // an ORBIS_ARENA_MIB below this is a bisect knob, not an arena worth placing

/// Bytes of RADV's window (the arena after the driver's private slice) that lie in the 4 GiB window its start is in: the part that can serve
/// RADEON_FLAG_32BIT allocations. 0 when the arena cannot even hold the private slice.
inline std::uint64_t Usable32(std::uint64_t base, std::uint64_t len) {
    if (len <= PrivateBytes) {
        return 0;
    }
    const std::uint64_t start = base + PrivateBytes;
    const std::uint64_t end = base + len;
    const std::uint64_t next_window = (start | (WindowBytes - 1)) + 1;
    return (end < next_window ? end : next_window) - start;
}

/// True when all of RADV's window lies in one 4 GiB window, i.e. every allocation can be a 32-bit one.
inline bool Whole(std::uint64_t base, std::uint64_t len) {
    return len > PrivateBytes && Usable32(base, len) == len - PrivateBytes;
}

/// ORBIS_ARENA_MIB as the driver reads it (strtoul, base 10); 0 when unset, empty or not a number.
inline std::uint64_t ArenaMiBFromEnv(const char* text) {
    if (text == nullptr || *text == '\0') {
        return 0;
    }
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 10);
    return end != text ? value : 0;
}

/// Whether a sceKernelMapDirectMemory call is the driver's request for its arena: the size the environment names, a free choice of address
/// (`*addr` null, no flags) and an arena worth placing. The driver's other direct-memory mappings (carve-outs of at most 512 MiB, the stream buffer)
/// are not of this size and address.
inline bool IsArenaRequest(std::uint64_t arena_mib, std::uint64_t len, std::int32_t flags, bool addr_free) {
    return arena_mib >= MinArenaMiB && addr_free && flags == 0 && len == (arena_mib << 20);
}

/// What the search found, for the log and for the tests.
struct Outcome {
    std::uint64_t first = 0;   // the range the kernel offered first (where the arena would have been)
    std::uint64_t chosen = 0;  // the first range that lies in one window; 0 when none did
    unsigned tries = 0;        // reservations asked for
    unsigned fillers = 0;      // ranges kept reserved because they straddled
    std::uint64_t filler[MaxTries] = {};
    bool refused = false;      // the kernel refused a reservation
};

/// Asks `reserve(len)` (address of a reserved range of len bytes, 0 when the kernel refuses) until a range lies in one window, at most `max_tries`
/// times. Every range that straddled stays reserved (listed in `filler`), so the next offer is a later one. The caller releases `chosen` just
/// before the driver's own request and the fillers after it.
template <class Reserve>
Outcome Search(Reserve&& reserve, std::uint64_t len, unsigned max_tries = MaxTries) {
    Outcome out;
    if (max_tries > MaxTries) {
        max_tries = MaxTries;
    }
    while (out.tries < max_tries) {
        const std::uint64_t at = reserve(len);
        if (at == 0) {
            out.refused = true;
            break;
        }
        if (out.tries == 0) {
            out.first = at;
        }
        ++out.tries;
        if (Whole(at, len)) {
            out.chosen = at;
            break;
        }
        out.filler[out.fillers++] = at;
    }
    return out;
}

/// arena_place.cpp (the interception; the PC tests do not link it): main() arms it before the game is loaded, and asks afterwards whether the
/// driver's request for its arena went through it.
void Arm(bool on);
bool Reached();

inline std::string Hex(std::uint64_t value) {
    char text[24];
    std::snprintf(text, sizeof text, "0x%llx", static_cast<unsigned long long>(value));
    return text;
}

/// The boot.log line for a search, `final_at` being where the driver's arena really was mapped (0 when not known yet).
inline std::string Describe(const Outcome& out, std::uint64_t len, std::uint64_t final_at) {
    const unsigned long long mib = static_cast<unsigned long long>(len >> 20);
    char line[640];
    if (out.first == 0) {
        std::snprintf(line, sizeof line, "arena placement: the kernel refused to reserve %llu MiB of address space to look at, so the arena is left where the kernel puts it", mib);
    } else if (out.fillers == 0 && out.chosen != 0) {
        std::snprintf(line, sizeof line, "arena placement: the kernel's first offer for the %llu MiB arena, %s, lies in one 4 GiB window (window 0x%llx); nothing to move",
                      mib, Hex(out.first).c_str(), static_cast<unsigned long long>(out.first >> 32));
    } else if (out.chosen != 0) {
        std::snprintf(line, sizeof line,
                      "arena placement: the kernel's first offer for the %llu MiB arena, %s, straddles a 4 GiB boundary (only %llu MiB of %llu MiB could serve 32-bit allocations); "
                      "%u reservation(s) of address space moved it to %s, which lies in window 0x%llx",
                      mib, Hex(out.first).c_str(), static_cast<unsigned long long>(Usable32(out.first, len) >> 20),
                      static_cast<unsigned long long>((len - PrivateBytes) >> 20), out.fillers, Hex(out.chosen).c_str(),
                      static_cast<unsigned long long>(out.chosen >> 32));
    } else {
        std::snprintf(line, sizeof line,
                      "arena placement: the kernel's first offer for the %llu MiB arena, %s, straddles a 4 GiB boundary and %u more %s did not help%s; the arena is left where the kernel puts it",
                      mib, Hex(out.first).c_str(), out.tries > 0 ? out.tries - 1 : 0u, out.tries == 2 ? "offer" : "offers",
                      out.refused ? " (the kernel then refused to reserve more)" : "");
    }
    std::string text = line;
    if (final_at != 0) {
        char tail[200];
        std::snprintf(tail, sizeof tail, "; the driver's arena was mapped at %s (%s)", Hex(final_at).c_str(),
                      Whole(final_at, len) ? "inside one window" : "STILL across a boundary");
        text += tail;
    }
    return text;
}

} // namespace Ps4ArenaPlace
