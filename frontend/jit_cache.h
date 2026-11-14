// SPDX-License-Identifier: GPL-3.0-or-later
// eden-ps4 0.9.4: the size of the JIT code cache of each emulated core, and how often a whole cache was thrown away. Both are in Eden patch 0031
// (patches/eden/): arm_dynarmic_64.cpp / _32.cpp read ps4_jit_cache_mib when they make a core, and Dynarmic's A64 interface counts the two ways
// a whole flush happens. This file defines the symbols (weak, like the JIT pack's; Eden only declares them) and holds the pure logic around them,
// which is tested on a PC (tests/jit_cache_test.cpp).
//
// Why it matters: a cache that fills up is emptied whole and every block is compiled again, which on this CPU shows up as a stall of a second or
// more. 32 MiB per core was chosen in test 5 when memory ran out; the console now measures 4.3 GiB of direct memory ("roomy"). The 0.9.4 test log
// of Mario vs. Donkey Kong answered the question: at 32 MiB the caches were thrown away 6 times in 200 s, with freezes of 1.5 to 5.7 s and 6 to 12
// FPS for tens of seconds after each. 0.9.4 fix 2: "auto" is now 64 MiB per core on a roomy console (still 32 elsewhere and in safe mode), and
// Eden patch 0032 takes the caches on 2 MiB boundaries (large pages) and counts compiled blocks and invalidated ranges, which boot.log shows.

#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>

extern "C" {
__attribute__((weak)) unsigned ps4_jit_cache_mib = 0; // 0 = not set: Eden uses 32
__attribute__((weak)) unsigned long long ps4_jit_cache_full = 0;  // whole flushes because the cache was full
__attribute__((weak)) unsigned long long ps4_jit_cache_asked = 0; // whole flushes because the game asked (IC IALLU)
// Eden patch 0032:
__attribute__((weak)) unsigned long long ps4_jit_cache_large = 0;        // code caches that got a block on 2 MiB boundaries (large pages possible)
__attribute__((weak)) unsigned long long ps4_jit_cache_small = 0;        // code caches that had to settle for 16 KiB boundaries
__attribute__((weak)) unsigned long long ps4_jit_blocks_compiled = 0;    // blocks the JIT compiled (all cores)
__attribute__((weak)) unsigned long long ps4_jit_ranges_invalidated = 0; // cache ranges the emulator asked to invalidate (each halts a core)
}

namespace Ps4JitCache {

constexpr unsigned DefaultMiB = 32;
constexpr unsigned AutoRoomyMiB = 64; // "auto" on a roomy console (0.9.4 fix 2)
constexpr unsigned MinMiB = 16;
constexpr unsigned MaxMiB = 256;

/// settings.txt jit_cache_mib=auto|16..256. "auto" is stored as 0 (ForStart decides what it means); other text is not a value (the line is ignored).
inline bool Parse(const std::string& value, unsigned* out) {
    if (value == "auto") {
        *out = 0;
        return true;
    }
    if (value.empty() || value.size() > 3) {
        return false;
    }
    for (char c : value) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    const unsigned n = static_cast<unsigned>(std::atoi(value.c_str()));
    if (n < MinMiB || n > MaxMiB) {
        return false;
    }
    *out = n;
    return true;
}

/// What goes into ps4_jit_cache_mib for this start (0 = Eden's 32). A value in settings.txt is used as it is. With "auto" a console with room for it
/// ("roomy": 4.3 GiB of direct memory or more) gets AutoRoomyMiB; any other console, and a start after a death before drawing (safe mode, when the
/// memory may have been the cause), keep 32.
inline unsigned ForStart(unsigned requested, bool roomy, bool safe_mode) {
    if (requested != 0) {
        return requested;
    }
    return (roomy && !safe_mode) ? AutoRoomyMiB : 0;
}

/// What the core will really use for a requested value (the same clamp as Eden patch 0031).
inline unsigned Effective(unsigned requested) {
    const unsigned mib = requested == 0 ? DefaultMiB : requested;
    return mib < MinMiB ? MinMiB : (mib > MaxMiB ? MaxMiB : mib);
}

struct Counts {
    unsigned long long full = 0;
    unsigned long long asked = 0;
    bool operator==(const Counts& o) const { return full == o.full && asked == o.asked; }
    bool operator!=(const Counts& o) const { return !(*this == o); }
};

inline Counts Read() {
    return {ps4_jit_cache_full, ps4_jit_cache_asked};
}

/// The line for boot.log. 'requested' is what settings.txt asked for (0 = auto); 'used' is what ForStart made of it (0 = Eden's 32).
inline std::string Describe(unsigned requested, unsigned used, const Counts& c) {
    char line[360];
    const unsigned mib = Effective(used);
    if (c.full == 0 && c.asked == 0) {
        const char* why = requested != 0 ? "jit_cache_mib in settings.txt"
                          : mib > DefaultMiB ? "automatic on this console; jit_cache_mib=128 in settings.txt gives more room"
                                             : "default; jit_cache_mib=64 or 128 in settings.txt gives more room";
        std::snprintf(line, sizeof line, "JIT cache: %u MiB per core (%s); never thrown away whole so far", mib, why);
        return line;
    }
    std::snprintf(line, sizeof line,
                  "JIT cache: %u MiB per core; thrown away whole %llu time(s) because it was full and %llu time(s) because the game asked%s", mib, c.full, c.asked,
                  c.full == 0 ? "" : mib < 128 ? " - each time every block is compiled again (a stall); jit_cache_mib=128 in settings.txt would give it room"
                                               : " - each time every block is compiled again (a stall)");
    return line;
}

/// How the cores' code caches were taken (Eden patch 0032), once they exist; empty before that.
inline std::string DescribePages(unsigned long long large, unsigned long long small) {
    if (large + small == 0) {
        return {};
    }
    char line[240];
    std::snprintf(line, sizeof line,
                  "JIT cache memory: %llu core cache(s) on 2 MiB boundaries (the kernel can use large pages for them), %llu on 16 KiB boundaries (the block the kernel gave when no 2 MiB one was left)",
                  large, small);
    return line;
}

struct Activity {
    unsigned long long blocks = 0;
    unsigned long long ranges = 0;
    bool operator==(const Activity& o) const { return blocks == o.blocks && ranges == o.ranges; }
    bool operator!=(const Activity& o) const { return !(*this == o); }
};

inline Activity ReadActivity() {
    return {ps4_jit_blocks_compiled, ps4_jit_ranges_invalidated};
}

/// What the JIT did since the last time (one status period): compiling is what fills the cache.
inline std::string DescribeActivity(const Activity& before, const Activity& now, unsigned seconds) {
    const unsigned long long blocks = now.blocks >= before.blocks ? now.blocks - before.blocks : 0;
    const unsigned long long ranges = now.ranges >= before.ranges ? now.ranges - before.ranges : 0;
    char line[240];
    std::snprintf(line, sizeof line,
                  "JIT work in the last %u s: %llu block(s) compiled, %llu cache range(s) invalidated (%llu and %llu since the start)", seconds, blocks, ranges,
                  now.blocks, now.ranges);
    return line;
}

/// The whole game, for the line written when it ends.
inline std::string DescribeTotals(const Activity& a) {
    char line[200];
    std::snprintf(line, sizeof line, "JIT work over the whole game: %llu block(s) compiled, %llu cache range(s) invalidated", a.blocks, a.ranges);
    return line;
}

} // namespace Ps4JitCache
