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
        Check(roomy.arena_mib == 1280 && roomy.shader_preload == 256 && roomy.driver_cache && !roomy.vram_in_arena,
              "auto on a roomy console keeps exactly the 0.7.3 / 0.7.4 values");
        const Plan standard = PlanFor(Profile::Auto, Tier::Standard);
        const Plan tight = PlanFor(Profile::Auto, Tier::Tight);
        Check(standard.arena_mib < roomy.arena_mib && tight.arena_mib < standard.arena_mib, "less memory: a smaller GPU arena");
        Check(tight.shader_preload <= standard.shader_preload && standard.shader_preload < roomy.shader_preload, "...and a smaller shader preload");
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
    std::printf(g_failures == 0 ? "memory guard test: PASSED\n" : "memory guard test: FAILED\n");
    return g_failures == 0 ? 0 : 1;
}
