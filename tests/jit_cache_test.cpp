// SPDX-License-Identifier: GPL-3.0-or-later
// Host test of frontend/jit_cache.h (0.9.4): the settings value, the clamp Eden patch 0031 applies, and the boot.log wording.
//   g++ -std=c++20 -Wall -Wextra -Ifrontend tests/jit_cache_test.cpp -o jit_cache_test && ./jit_cache_test
#include <cstdio>
#include <string>

#include "jit_cache.h"

namespace {
int g_failures = 0;
void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    g_failures += ok ? 0 : 1;
}
bool Has(const std::string& text, const char* part) {
    return text.find(part) != std::string::npos;
}
} // namespace

int main() {
    unsigned v = 99;
    Check(Ps4JitCache::Parse("auto", &v) && v == 0, "auto is 0 (not set: Eden keeps 32 MiB)");
    Check(Ps4JitCache::Parse("64", &v) && v == 64, "64 is 64");
    Check(Ps4JitCache::Parse("16", &v) && v == 16 && Ps4JitCache::Parse("256", &v) && v == 256, "16 and 256, the two ends, are values");
    v = 7;
    Check(!Ps4JitCache::Parse("15", &v) && !Ps4JitCache::Parse("257", &v) && !Ps4JitCache::Parse("0", &v) && v == 7, "outside 16..256 is not a value and leaves the variable alone");
    Check(!Ps4JitCache::Parse("", &v) && !Ps4JitCache::Parse("12a", &v) && !Ps4JitCache::Parse("-64", &v) && !Ps4JitCache::Parse("1000", &v) && !Ps4JitCache::Parse("on", &v),
          "text, signs, empty and four digits are not values");
    Check(Ps4JitCache::Effective(0) == 32 && Ps4JitCache::Effective(64) == 64 && Ps4JitCache::Effective(5) == 16 && Ps4JitCache::Effective(9999) == 256,
          "Effective is the clamp of the Eden patch: 0 -> 32, 16..256");

    // Nothing in this test defines the counters: the weak ones are all zero, as when no game has flushed.
    const Ps4JitCache::Counts none = Ps4JitCache::Read();
    Check(none.full == 0 && none.asked == 0 && none == Ps4JitCache::Counts{}, "counters start at zero");
    const std::string quiet = Ps4JitCache::Describe(0, none);
    Check(Has(quiet, "32 MiB per core") && Has(quiet, "never thrown away") && Has(quiet, "jit_cache_mib=64 or 128"), "default, never flushed: says 32 MiB and how to get more");
    const std::string set = Ps4JitCache::Describe(128, none);
    Check(Has(set, "128 MiB per core") && Has(set, "never thrown away") && !Has(set, "jit_cache_mib=64 or 128"), "set to 128, never flushed: no hint to raise it");
    const std::string hit = Ps4JitCache::Describe(0, {3, 1});
    Check(Has(hit, "3 time(s) because it was full") && Has(hit, "1 time(s) because the game asked") && Has(hit, "compiled again"), "flushes: both counts and the consequence");
    Check(!Has(Ps4JitCache::Describe(64, {0, 2}), "compiled again"), "only the game asking: no advice to raise the size (a bigger cache would not help)");
    Check((Ps4JitCache::Counts{1, 0}) != (Ps4JitCache::Counts{0, 1}) && (Ps4JitCache::Counts{2, 2}) == (Ps4JitCache::Counts{2, 2}), "Counts compares both fields");

    std::printf("jit_cache_test: %s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
