// SPDX-License-Identifier: GPL-3.0-or-later
// Host test of frontend/jit_cache.h (0.9.4): the settings value, the clamp Eden patch 0031 applies, and the boot.log wording; and (0.9.7) of
// frontend/texture_traffic.h, the other header of weak counters that Eden patches fill (patch 0035).
//   g++ -std=c++20 -Wall -Wextra -Ifrontend tests/jit_cache_test.cpp -o jit_cache_test && ./jit_cache_test
#include <cstdio>
#include <string>

#include "jit_cache.h"
#include "texture_traffic.h"

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

    // 0.9.7, Eden patch 0034: the code caches take their direct memory in 2 MiB steps as the code grows
    bool lazy = false;
    Check(Ps4JitCache::ParseCommit("lazy", &lazy) && lazy && Ps4JitCache::ParseCommit("full", &lazy) && !lazy, "jit_cache_commit: lazy and full are the two values");
    Check(Ps4JitCache::ParseCommit("on", &lazy) && lazy && Ps4JitCache::ParseCommit("off", &lazy) && !lazy && Ps4JitCache::ParseCommit("auto", &lazy) && lazy,
          "jit_cache_commit: on/off/auto are accepted spellings");
    lazy = true;
    Check(!Ps4JitCache::ParseCommit("", &lazy) && !Ps4JitCache::ParseCommit("maybe", &lazy) && !Ps4JitCache::ParseCommit("LAZY", &lazy) && lazy, "other text is not a value and leaves the variable alone");
    Check(Ps4JitCache::LazyForStart(true, false) && !Ps4JitCache::LazyForStart(true, true) && !Ps4JitCache::LazyForStart(false, false) && !Ps4JitCache::LazyForStart(false, true),
          "lazy steps: on only when asked for and not in safe mode");
    Check(Ps4JitCache::ForStart(0, true, false, true) == 256 && Ps4JitCache::ForStart(0, false, false, true) == 128, "auto with lazy steps: a 256 MiB ceiling on a roomy console, 128 elsewhere");
    Check(Ps4JitCache::ForStart(0, true, true, true) == 0 && Ps4JitCache::ForStart(0, false, true, true) == 0, "auto in safe mode: 32 (0) even when lazy was asked for");
    Check(Ps4JitCache::ForStart(0, true, false, false) == 64 && Ps4JitCache::ForStart(0, false, false, false) == 0, "auto without lazy steps: what 0.9.6 did (64 roomy, 32 otherwise)");
    Check(Ps4JitCache::ForStart(96, true, false, true) == 96 && Ps4JitCache::ForStart(96, false, true, true) == 96, "a value in settings.txt wins over lazy and over safe mode");
    Check(Ps4JitCache::Effective(Ps4JitCache::ForStart(0, true, false, true)) == 256 && Ps4JitCache::Effective(Ps4JitCache::ForStart(0, false, false, true)) == 128,
          "the ceilings are values the core accepts as they are");
    const Ps4JitCache::Commit nothing = Ps4JitCache::ReadCommit();
    Check(nothing == Ps4JitCache::Commit{} && Ps4JitCache::DescribeCommit(true, nothing).empty() && Ps4JitCache::DescribeCommit(false, nothing).empty(), "no cache made yet: nothing to say");
    Ps4JitCache::Commit grown;
    grown.reserved = 4ull * 256 << 20;
    grown.committed = 4ull * 6 << 20;
    grown.large = 12;
    grown.small = 0;
    const std::string lazy_line = Ps4JitCache::DescribeCommit(true, grown);
    Check(Has(lazy_line, "24 MiB held now of 1024 MiB reserved") && Has(lazy_line, "2 MiB steps as the code grows") && Has(lazy_line, "12 step(s) on 2 MiB boundaries, 0 on 16 KiB") &&
              !Has(lazy_line, "REFUSED"),
          "lazy line: what is held, what is reserved, how the steps were taken");
    grown.failed = 2;
    Check(Has(Ps4JitCache::DescribeCommit(true, grown), "THE KERNEL REFUSED A STEP") && Has(Ps4JitCache::DescribeCommit(true, grown), "emptied whole when it is full"),
          "a refused step is said loudly, with what it means");
    grown.failed = 0;
    const std::string whole_line = Ps4JitCache::DescribeCommit(false, grown);
    Check(Has(whole_line, "taken whole") && Has(whole_line, "24 MiB of direct memory held from the start") && !Has(whole_line, "2 MiB steps"), "whole line: says it was taken whole");
    Check((Ps4JitCache::Commit{1, 2, 3, 4, 5}) == (Ps4JitCache::Commit{1, 2, 9, 9, 5}) && (Ps4JitCache::Commit{1, 2, 0, 0, 0}) != (Ps4JitCache::Commit{1, 3, 0, 0, 0}) &&
              (Ps4JitCache::Commit{1, 2, 0, 0, 0}) != (Ps4JitCache::Commit{1, 2, 0, 0, 1}),
          "Commit compares what the log line shows (reserved, held, refused), not the step counts alone");

    // 0.9.7, Eden patch 0035: texture traffic (diagnostics)
    namespace TT = Ps4TextureTraffic;
    // 0.9.7: the automatic size is 256 (roomy) or 128 with lazy steps; the hint must not point at a size that is not larger
    Check(Has(Ps4JitCache::Describe(0, 256, {}), "256 MiB per core") && Has(Ps4JitCache::Describe(0, 256, {}), "the most the setting gives") && !Has(Ps4JitCache::Describe(0, 256, {}), "gives more room"),
          "automatic 256: no hint to raise it");
    Check(Has(Ps4JitCache::Describe(0, 128, {}), "jit_cache_mib=256 in settings.txt gives more room") && !Has(Ps4JitCache::Describe(0, 128, {}), "jit_cache_mib=128"),
          "automatic 128: the hint points at 256");
    const TT::Counts idle = TT::Read();
    Check(idle == TT::Counts{} && TT::Describe(idle, idle, 10).empty() && TT::DescribeTotals(idle).empty(), "texture traffic: nothing counted, nothing said");
    Check(TT::Size(0) == "0 B" && TT::Size(1023) == "1023 B" && TT::Size(1024) == "1 KiB" && TT::Size(300 * 1024) == "300 KiB" && TT::Size(1ull << 20) == "1.0 MiB" &&
              TT::Size(5ull << 19) == "2.5 MiB",
          "texture traffic: sizes in B, KiB and MiB");
    TT::Counts a0;
    a0.uploads = 10;
    a0.upload_bytes = 40ull << 20;
    a0.largest = 8ull << 20;
    TT::Counts a1 = a0;
    a1.uploads = 22;
    a1.upload_bytes = 70ull << 20;
    a1.largest = 16ull << 20;
    a1.swizzles = 5;
    a1.swizzle_bytes = 3ull << 20;
    const std::string traffic = TT::Describe(a0, a1, 10);
    Check(Has(traffic, "last 10 s") && Has(traffic, "12 image upload(s), 30.0 MiB") && Has(traffic, "the largest so far 16.0 MiB") && Has(traffic, "5 pitch->block-linear (3.0 MiB)") &&
              Has(traffic, "0 block-linear->pitch (0 B)") && Has(traffic, "0 block-linear->block-linear (0 B)"),
          "texture traffic: the difference over the period, the largest so far, and the three kinds of CPU copy");
    Check(Has(TT::Describe(a1, a0, 10), "0 image upload(s), 0 B") == false && TT::Describe(a1, a0, 10).empty(), "texture traffic: counters that went backwards give nothing, not a wrapped number");
    TT::Counts only_copy;
    only_copy.bl_copies = 2;
    only_copy.bl_copy_bytes = 512 * 1024;
    Check(Has(TT::Describe({}, only_copy, 10), "0 image upload(s)") && Has(TT::Describe({}, only_copy, 10), "2 block-linear->block-linear (512 KiB)"),
          "texture traffic: copies without uploads are reported too");
    const std::string whole = TT::DescribeTotals(a1);
    Check(Has(whole, "whole game: 22 image upload(s), 70.0 MiB (the largest 16.0 MiB)") && Has(whole, "5 pitch->block-linear (3.0 MiB)"), "texture traffic: the totals line for the end of the game");
    Check(a0 != a1 && a1 == a1 && (TT::Counts{}) == (TT::Counts{}), "texture traffic: Counts compares every field");

    std::printf("jit_cache_test: %s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
