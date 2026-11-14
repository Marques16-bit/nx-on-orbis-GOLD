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

    // 0.9.4 fix 2: what "auto" means
    Check(Ps4JitCache::ForStart(0, true, false) == 64 && Ps4JitCache::ForStart(0, false, false) == 0 && Ps4JitCache::ForStart(0, true, true) == 0,
          "auto: 64 MiB on a roomy console, 32 (0) on any other and in safe mode");
    Check(Ps4JitCache::ForStart(128, false, false) == 128 && Ps4JitCache::ForStart(256, true, false) == 256 && Ps4JitCache::ForStart(16, true, false) == 16,
          "a value in settings.txt is used as it is, roomy or not");

    // Nothing in this test defines the counters: the weak ones are all zero, as when no game has flushed.
    const Ps4JitCache::Counts none = Ps4JitCache::Read();
    Check(none.full == 0 && none.asked == 0 && none == Ps4JitCache::Counts{}, "counters start at zero");
    const std::string quiet = Ps4JitCache::Describe(0, 0, none);
    Check(Has(quiet, "32 MiB per core") && Has(quiet, "never thrown away") && Has(quiet, "jit_cache_mib=64 or 128"), "default, never flushed: says 32 MiB and how to get more");
    const std::string automatic = Ps4JitCache::Describe(0, 64, none);
    Check(Has(automatic, "64 MiB per core") && Has(automatic, "automatic on this console") && Has(automatic, "jit_cache_mib=128") && !Has(automatic, "64 or 128"),
          "auto on a roomy console: says it is automatic and offers 128");
    const std::string set = Ps4JitCache::Describe(128, 128, none);
    Check(Has(set, "128 MiB per core") && Has(set, "never thrown away") && Has(set, "jit_cache_mib in settings.txt") && !Has(set, "gives more room"), "set to 128, never flushed: no hint to raise it");
    const std::string hit = Ps4JitCache::Describe(0, 0, {3, 1});
    Check(Has(hit, "3 time(s) because it was full") && Has(hit, "1 time(s) because the game asked") && Has(hit, "compiled again") && Has(hit, "jit_cache_mib=128"),
          "flushes at 32: both counts, the consequence and the advice");
    Check(Has(Ps4JitCache::Describe(0, 128, {2, 0}), "compiled again") && !Has(Ps4JitCache::Describe(0, 128, {2, 0}), "jit_cache_mib=128"),
          "flushes even at 128: the consequence, and no advice to set what is already set");
    Check(!Has(Ps4JitCache::Describe(64, 64, {0, 2}), "compiled again"), "only the game asking: no advice to raise the size (a bigger cache would not help)");
    Check((Ps4JitCache::Counts{1, 0}) != (Ps4JitCache::Counts{0, 1}) && (Ps4JitCache::Counts{2, 2}) == (Ps4JitCache::Counts{2, 2}), "Counts compares both fields");

    // Eden patch 0032: how the caches were taken, and what the JIT did
    Check(Ps4JitCache::DescribePages(0, 0).empty(), "no cache made yet: nothing to say");
    const std::string pages = Ps4JitCache::DescribePages(3, 1);
    Check(Has(pages, "3 core cache(s) on 2 MiB boundaries") && Has(pages, "1 on 16 KiB boundaries"), "pages: both counts");
    Check(Has(Ps4JitCache::DescribePages(0, 4), "0 core cache(s) on 2 MiB") && Has(Ps4JitCache::DescribePages(0, 4), "4 on 16 KiB"), "pages: all on 16 KiB says so");
    const std::string work = Ps4JitCache::DescribeActivity({100, 5}, {1100, 9}, 10);
    Check(Has(work, "last 10 s") && Has(work, "1000 block(s) compiled") && Has(work, "4 cache range(s) invalidated") && Has(work, "(1100 and 9 since the start)"),
          "activity: the difference over the period and the totals");
    Check(Has(Ps4JitCache::DescribeActivity({50, 50}, {10, 10}, 10), "0 block(s) compiled"), "counters that went backwards (never) do not wrap around");
    Check(Has(Ps4JitCache::DescribeTotals({7, 3}), "whole game: 7 block(s) compiled, 3 cache range(s) invalidated"), "totals line for the end of the game");
    Check((Ps4JitCache::Activity{1, 2}) == (Ps4JitCache::Activity{1, 2}) && (Ps4JitCache::Activity{1, 2}) != (Ps4JitCache::Activity{1, 3}), "Activity compares both fields");

    std::printf("jit_cache_test: %s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
