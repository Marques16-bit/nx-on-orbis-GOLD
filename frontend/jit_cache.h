// SPDX-License-Identifier: GPL-3.0-or-later
// eden-ps4 0.9.4: the size of the JIT code cache of each emulated core, and how often a whole cache was thrown away. Both are in Eden patch 0031
// (patches/eden/): arm_dynarmic_64.cpp / _32.cpp read ps4_jit_cache_mib when they make a core, and Dynarmic's A64 interface counts the two ways
// a whole flush happens. This file defines the symbols (weak, like the JIT pack's; Eden only declares them) and holds the pure logic around them,
// which is tested on a PC (tests/jit_cache_test.cpp).
//
// Why it matters: a cache that fills up is emptied whole and every block is compiled again, which on this CPU shows up as a stall of a second or
// more. 32 MiB per core was chosen in test 5 when memory ran out; the console now measures 4.3 GiB of direct memory ("roomy"), and whether any
// game fills 32 MiB was not known. boot.log now says.

#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>

extern "C" {
__attribute__((weak)) unsigned ps4_jit_cache_mib = 0; // 0 = not set: Eden uses 32
__attribute__((weak)) unsigned long long ps4_jit_cache_full = 0;  // whole flushes because the cache was full
__attribute__((weak)) unsigned long long ps4_jit_cache_asked = 0; // whole flushes because the game asked (IC IALLU)
}

namespace Ps4JitCache {

constexpr unsigned DefaultMiB = 32;
constexpr unsigned MinMiB = 16;
constexpr unsigned MaxMiB = 256;

/// settings.txt jit_cache_mib=auto|16..256. "auto" (and 0) leave it at the default 32; other text is not a value (the line is ignored).
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

/// The line for boot.log. 'requested' is what settings.txt asked for (0 = auto).
inline std::string Describe(unsigned requested, const Counts& c) {
    char line[320];
    const unsigned mib = Effective(requested);
    if (c.full == 0 && c.asked == 0) {
        std::snprintf(line, sizeof line, "JIT cache: %u MiB per core (%s); never thrown away whole so far", mib,
                      requested == 0 ? "default, settings.txt jit_cache_mib=64 or 128 gives more room" : "jit_cache_mib");
        return line;
    }
    std::snprintf(line, sizeof line,
                  "JIT cache: %u MiB per core; thrown away whole %llu time(s) because it was full and %llu time(s) because the game asked%s", mib, c.full, c.asked,
                  c.full > 0 ? " - each time every block is compiled again (a stall); jit_cache_mib=128 in settings.txt would give it room" : "");
    return line;
}

} // namespace Ps4JitCache
