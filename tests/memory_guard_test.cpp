// SPDX-License-Identifier: GPL-3.0-or-later
// Host test of frontend/memory_guard.cpp: the tiers, the profiles' plans, and the watch's stages,
// hysteresis and speed of fall.
//   g++ -std=c++20 -Ifrontend tests/memory_guard_test.cpp frontend/memory_guard.cpp -o memory_guard_test
#include <cstdio>
#include <string>

#include "memory_guard.h"

namespace {
int g_failures = 0;
void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    g_failures += ok ? 0 : 1;
}
} // namespace

int main() {
    using namespace Ps4Memory;
    Check(TierForBudget(4608) == Tier::Roomy && TierForBudget(4300) == Tier::Roomy, "4608 MiB (the PS4 Pro measured) and 4300 are roomy");
    Check(TierForBudget(4299) == Tier::Standard && TierForBudget(3800) == Tier::Standard, "3800 to 4299 is standard");
    Check(TierForBudget(3799) == Tier::Tight && TierForBudget(0) == Tier::Tight, "under 3800 (and an unreadable 0) is tight");

    {
        const Plan roomy = PlanFor(Profile::Auto, Tier::Roomy);
        Check(roomy.arena_mib == 1280 && roomy.shader_preload == -1 && roomy.driver_cache && !roomy.vram_in_arena,
              "auto on a roomy console: the 1280 MiB arena of 0.7.3 to 0.7.6 and no shader cache limit (0.7.7)");
        const Plan standard = PlanFor(Profile::Auto, Tier::Standard);
        const Plan tight = PlanFor(Profile::Auto, Tier::Tight);
        Check(standard.arena_mib <= roomy.arena_mib && tight.arena_mib < standard.arena_mib, "less memory: a smaller or equal GPU arena");
        Check(tight.arena_mib >= 1152, "...never the 1024 MiB that died loading Super Mario Maker 2's shaders");
        Check(tight.shader_preload > 0 && tight.shader_preload <= standard.shader_preload && standard.shader_preload > 0 && roomy.shader_preload < 0,
              "...and a smaller shader preload (roomy: -1 = all)");
        Check(!tight.driver_cache && standard.driver_cache, "...and the driver's pipeline cache is off only when tight");
        Check(PlanFor(Profile::Tight, Tier::Roomy).arena_mib == tight.arena_mib, "a profile other than auto ignores what was detected");
        Check(PlanFor(Profile::Roomy, Tier::Tight).arena_mib == 1280, "...in both directions");
        const Plan strict = PlanFor(Profile::Strict, Tier::Roomy);
        Check(strict.vram_in_arena && strict.arena_mib == tight.arena_mib && !PlanFor(Profile::Tight, Tier::Roomy).vram_in_arena,
              "strict is tight plus every GPU image inside the arena, and only strict does that");
        Profile p;
        Check(ParseProfile("strict", &p) && p == Profile::Strict && ParseProfile("auto", &p) && p == Profile::Auto && !ParseProfile("big", &p),
              "profile names parse");
        Action a;
        Check(ParseAction("pause", &a) && a == Action::Pause && ParseAction("warn", &a) && a == Action::Warn && !ParseAction("exit", &a),
              "action names parse (warn, pause)");
    }
    {
        Watch w;
        std::uint64_t t = 0;
        Check(w.Update(600, t).stage == Stage::Ok, "600 MiB free: ok");
        t += 1000;
        Check(w.Update(159, t).stage == Stage::Low, "below 160: low");
        t += 1000;
        Check(w.Update(170, t).stage == Stage::Low, "back to 170: still low (hysteresis, so a block hovering at 160 does not flap)");
        t += 1000;
        Check(w.Update(200, t).stage == Stage::Ok, "200: ok again");
        t += 1000;
        const Status crit = w.Update(63, t);
        Check(crit.stage == Stage::Critical && crit.entered_critical, "below 64: critical, flagged once");
        t += 1000;
        const Status again = w.Update(60, t);
        Check(again.stage == Stage::Critical && !again.entered_critical, "...and not flagged again while it stays there");
        t += 1000;
        Check(w.Update(80, t).stage == Stage::Critical, "80 MiB: still critical (needs 64 + 32)");
        t += 1000;
        Check(w.Update(100, t).stage == Stage::Low, "100 MiB: back to low");
    }
    {
        // A healthy race sits near 100 MiB for 15 minutes (docs/STATUS.md): low, never critical, no panic.
        Watch w;
        bool critical = false;
        for (int i = 0; i < 900; ++i) {
            critical = critical || w.Update(95 + std::uint64_t(i % 7), std::uint64_t(i) * 1000).stage == Stage::Critical;
        }
        Check(!critical && w.stage() == Stage::Low, "a steady ~100 MiB for 15 minutes is low but never critical");
    }
    {
        // A falling block: 400 MiB, losing 60 MiB a minute (1 MiB a second).
        Watch w;
        Status s;
        for (int i = 0; i <= 40; ++i) {
            s = w.Update(400 - std::uint64_t(i), std::uint64_t(i) * 1000);
        }
        Check(s.stage == Stage::Ok && s.trend_mib_per_min < -50 && s.trend_mib_per_min > -70, "speed of the fall: about -60 MiB/min");
        Check(s.eta_seconds > 0 && s.eta_seconds < 360 * 60 / 60 + 60, "ETA to the critical level is given (about 5 minutes)");
        Watch no_history;
        const Status early = no_history.Update(300, 0);
        const Status early2 = no_history.Update(250, 3000);
        Check(early.eta_seconds == -1 && early2.eta_seconds == -1, "under 10 s of history: no speed, no ETA");
        Watch steady;
        Status st;
        for (int i = 0; i <= 40; ++i) {
            st = steady.Update(300, std::uint64_t(i) * 1000);
        }
        Check(st.eta_seconds == -1 && st.trend_mib_per_min == 0, "a steady block: no ETA");
        Watch rising;
        for (int i = 0; i <= 40; ++i) {
            st = rising.Update(200 + std::uint64_t(i), std::uint64_t(i) * 1000);
        }
        Check(st.eta_seconds == -1, "a block that grows: no ETA");
    }
    {
        Status ok;
        Check(BannerText(ok, true).empty(), "stage ok: no text");
        Status low;
        low.stage = Stage::Low;
        low.free_mib = 120;
        Check(BannerText(low, false) == "LOW MEMORY: 120 MiB free", "low, no speed known");
        low.eta_seconds = 90;
        low.trend_mib_per_min = -20;
        const std::string pt = BannerText(low, true);
        Check(pt.find("MEMORIA BAIXA") != std::string::npos && pt.find("20 MiB/min") != std::string::npos && pt.find("90 s") != std::string::npos,
              "low and falling: the speed and the ETA are in the text");
        Status crit;
        crit.stage = Stage::Critical;
        crit.free_mib = 40;
        Check(BannerText(crit, false).find("CRITICAL") != std::string::npos && BannerText(crit, true).find("CRITICA") != std::string::npos,
              "critical text in both languages");
    }
    {
        // 0.9.0: sysflex_mib and the budget the console probe turns it into.
        std::uint64_t cap = 99;
        Check(ParseSystemFlexibleCap("off", &cap) && cap == 0 && ParseSystemFlexibleCap("128", &cap) && cap == 128 &&
                  ParseSystemFlexibleCap("256", &cap) && cap == 256 && ParseSystemFlexibleCap("384", &cap) && cap == 384 &&
                  ParseSystemFlexibleCap("512", &cap) && cap == 512,
              "sysflex_mib: off, 128, 256, 384 and 512 parse (the 0.9.0 values still work)");
        Check(ParseSystemFlexibleCap("768", &cap) && cap == 768 && ParseSystemFlexibleCap("1024", &cap) && cap == 1024 &&
                  ParseSystemFlexibleCap("1536", &cap) && cap == 1536 && ParseSystemFlexibleCap("2048", &cap) && cap == 2048,
              "sysflex_mib (0.9.1): 768, 1024, 1536 and 2048 parse");
        Check(ParseSystemFlexibleCap("auto", &cap) && cap == SystemFlexibleAutoCapMiB && SystemFlexibleAutoCapMiB == 1536,
              "sysflex_mib=auto is the automatic cap, 1536 MiB");
        Check(SystemFlexibleProvenMiB == 512 && SystemFlexibleProvenMiB < SystemFlexibleAutoCapMiB,
              "the proven amount (what safe mode falls back to) is 512 MiB, below auto");
        cap = 7;
        Check(!ParseSystemFlexibleCap("4096", &cap) && !ParseSystemFlexibleCap("", &cap) && !ParseSystemFlexibleCap("on", &cap) &&
                  !ParseSystemFlexibleCap("0", &cap) && !ParseSystemFlexibleCap("AUTO", &cap) && cap == 7,
              "sysflex_mib: anything else is refused and leaves the value alone");
        Check(SystemFlexibleBudgetMiB(0, 640) == 0, "budget: a cap of 0 is off whatever the pool has");
        Check(SystemFlexibleBudgetMiB(384, 640) == 384 && SystemFlexibleBudgetMiB(512, 640) == 512, "budget: a roomy pool gives the whole cap");
        Check(SystemFlexibleBudgetMiB(512, 480) == 352, "budget: a pool of 480 MiB gives 480 - 128 reserve = 352");
        Check(SystemFlexibleBudgetMiB(384, 300) == 160 && SystemFlexibleBudgetMiB(384, 301) == 160, "budget: rounded down to steps of 32 MiB");
        Check(SystemFlexibleBudgetMiB(256, 159) == 0 && SystemFlexibleBudgetMiB(256, 0) == 0, "budget: a pool with no room beyond the reserve plus one step is off");
        Check(SystemFlexibleBudgetMiB(256, 160) == 32, "budget: exactly reserve + one step leaves one step");
        Check(SystemFlexibleBudgetMiB(SystemFlexibleAutoCapMiB, 4096) == 1536 && SystemFlexibleBudgetMiB(2048, 2176) == 2048,
              "budget: auto and 2048 MiB fit a pool that can map cap + reserve at once");
        Check(SystemFlexibleBudgetMiB(SystemFlexibleAutoCapMiB, 1024) == 896, "budget: auto on a pool that maps 1024 MiB takes 896 and keeps 128");
        // The probe's search: the largest size in steps that the (mock) pool accepts at once.
        int asked = 0;
        const auto pool_of = [&](std::uint64_t limit) {
            asked = 0;
            return [&asked, limit](std::uint64_t mib) { ++asked; return mib <= limit; };
        };
        Check(LargestAcceptedMiB(1664, pool_of(4096)) == 1664 && asked == 1, "probe: a roomy pool answers the first question, one probe");
        Check(LargestAcceptedMiB(1664, pool_of(1000)) == 992 && asked <= 8, "probe: a pool of 1000 MiB is found as 992 (steps of 32) in at most 8 probes");
        Check(LargestAcceptedMiB(2176, pool_of(640)) == 640, "probe: 640 MiB exactly is found");
        Check(LargestAcceptedMiB(2176, pool_of(161)) == 160, "probe: the smallest size (reserve + step = 160) is found when nothing bigger fits");
        Check(LargestAcceptedMiB(2176, pool_of(159)) == 0 && LargestAcceptedMiB(2176, pool_of(0)) == 0, "probe: a pool under the smallest size is 0");
        Check(LargestAcceptedMiB(100, pool_of(4096)) == 0 && asked == 0, "probe: a maximum under the smallest size asks nothing");
        Check(LargestAcceptedMiB(160, pool_of(100)) == 0, "probe: a maximum equal to the smallest size, refused, is 0");
        {
            bool all_ok = true;
            for (std::uint64_t limit = 0; limit <= 2300; limit += 7) {
                const std::uint64_t want = limit < 160 ? 0 : std::min<std::uint64_t>(limit, 1664) / 32 * 32;
                all_ok = all_ok && LargestAcceptedMiB(1664, pool_of(limit)) == want;
            }
            Check(all_ok, "probe: for every pool size from 0 to 2300 MiB (steps of 7) the answer is the largest step the pool accepts, capped at the maximum");
        }
    }
    std::printf(g_failures == 0 ? "memory guard test: PASSED\n" : "memory guard test: FAILED\n");
    return g_failures == 0 ? 0 : 1;
}
