// SPDX-License-Identifier: GPL-3.0-or-later
// Host test of frontend/memory_guard.cpp: the tiers, the profiles' plans, and the watch's stages,
// hysteresis and speed of fall.
//   g++ -std=c++20 -Ifrontend tests/memory_guard_test.cpp frontend/memory_guard.cpp -o memory_guard_test
#include <cstdio>
#include <string>
#include <vector>

#include "arena_place.h"
#include "memory_guard.h"
#include "thread_placement.h"

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
    {
        // 0.9.6: the arena table.
        ArenaInputs in;
        in.direct_mib = 4608;
        in.gpu_arena_mib = 1280;
        in.jit_cache_mib = 64;
        in.sysflex_budget_mib = 1536;
        const std::vector<ArenaLine> table = BuildArenaTable(in);
        Check(table.size() == 4 && table[0].mib == 1280 && table[1].mib == 4 * 64 && table[2].mib == 128, "arenas: the GPU arena, 4 JIT caches and the stream buffer come first");
        Check(table[3].name == "Left" && table[3].mib == 4608 - 1280 - 256 - 128, "arenas: what is left is the rest of the direct memory");
        Check(table[3].note.find("1536 MiB from the system pool") != std::string::npos, "arenas: the note says the system pool's share is not taken from it");
        in.sysflex_budget_mib = 0;
        Check(BuildArenaTable(in)[3].note.find("system pool") == std::string::npos, "arenas: with the pool off the note does not mention it");
        in.direct_mib = 1000; // slices larger than the memory: nothing left, never a wrapped-round number
        Check(BuildArenaTable(in)[3].mib == 0, "arenas: when the slices do not fit, the rest is 0 (no unsigned wrap)");
        in.direct_mib = 4608;
        in.jit_cores = 0;
        Check(BuildArenaTable(in)[1].mib == 0, "arenas: no cores, no JIT cache");
        in.jit_cores = 4;
        // 0.9.7: with lazy steps the caches are reserved (a ceiling), and only their first two 2 MiB steps are taken at the start.
        in.jit_cache_mib = 256;
        in.jit_lazy = true;
        const std::vector<ArenaLine> lazy_table = BuildArenaTable(in);
        Check(lazy_table.size() == 4 && lazy_table[1].name == "JIT code caches" && lazy_table[1].mib == 4 * 4,
              "arenas, lazy code caches: only the first two 2 MiB steps of each of the 4 caches are taken at the start");
        Check(lazy_table[1].note.find("each of 4 caches may grow to 256 MiB") != std::string::npos && lazy_table[1].note.find("1024 MiB reserved") != std::string::npos &&
                  lazy_table[1].note.find("jit_cache_commit") != std::string::npos,
              "arenas, lazy code caches: the note says the ceiling, the reservation and the setting");
        Check(lazy_table[3].mib == 4608 - 1280 - 16 - 128 && lazy_table[3].note.find("the code caches as they grow") != std::string::npos,
              "arenas, lazy code caches: what is left is larger by what the ceiling no longer takes, and the note says the caches draw on it");
        in.jit_lazy = false;
        Check(BuildArenaTable(in)[1].mib == 4 * 256 && BuildArenaTable(in)[3].note.find("as they grow") == std::string::npos,
              "arenas, caches taken whole: the whole size is taken and the note does not say they grow");
        in.jit_lazy = true;
        in.jit_cores = 0;
        Check(BuildArenaTable(in)[1].mib == 0, "arenas, lazy code caches: no cores, nothing taken");
        in.jit_cores = 4;
        in.jit_cache_mib = 64;
        in.jit_lazy = false;
        const std::vector<std::string> lines = DescribeArenaTable(in);
        Check(lines.size() == 5 && lines[0].find("4608 MiB") != std::string::npos && lines[1].find("GPU driver arena") != std::string::npos &&
                  lines[4].find("Left") != std::string::npos,
              "arenas: boot.log gets a heading and one line per slice");
    }
    {
        // 0.9.6: thread placement (frontend/thread_placement.h).
        namespace PP = Ps4Placement;
        using PP::Mode;
        using PP::Role;
        using PP::GuestIndexOf;
        using PP::GuestCores;
        using PP::ParseMode;
        Mode mode = Mode::Off;
        Check(ParseMode("auto", &mode) && mode == Mode::Auto && ParseMode("off", &mode) && mode == Mode::Off && !ParseMode("max", &mode), "placement: auto and off are read, anything else is not");
        Check(GuestIndexOf("CPUCore_0") == 0 && GuestIndexOf("CPUCore_3") == 3 && GuestIndexOf("CPUCore_4") == -1 && GuestIndexOf("CPUCore_10") == -1 &&
                  GuestIndexOf("CPUCore_") == -1 && GuestIndexOf("CPUThread") == 0 && GuestIndexOf("VulkanWorker") == -1,
              "placement: the Switch's four cores are recognised by name (CPUThread is the single-core mode)");
        const PP::Plan six{0x3F}; // cores 0 to 5
        Check(six.Active() && six.Cores() == 6 && six.GuestMask(0) == 0x01 && six.GuestMask(3) == 0x08 && six.SecondaryMask() == 0x30, "placement: six cores: the guest on 0-3, the rest on 4-5");
        Check(six.For("CPUCore_2").role == Role::Guest && six.For("CPUCore_2").mask == 0x04, "placement: CPUCore_2 gets core 2 alone");
        const char* others[] = {"GPU", "VulkanWorker", "VulkanPresent", "AudioOutPS4", "AudioRenderSystemManager", "FS", "nvservices", "HostTiming", "ShaderBuilder"};
        bool others_ok = true;
        for (const char* name : others) {
            others_ok = others_ok && six.For(name).role == Role::Secondary && six.For(name).mask == 0x30;
        }
        Check(others_ok, "placement: every other named thread shares cores 4 and 5 (GPU, Vulkan, audio, services, builders)");
        Check(six.For("main").role == Role::Free && six.For("").role == Role::Free, "placement: the main thread and unnamed threads are left alone");
        const PP::Plan seven{0x7F}; // a console that offers a seventh core: still six
        Check(seven.Cores() == 6 && seven.SecondaryMask() == 0x30, "placement: never more than six cores are used");
        const PP::Plan five{0x1F};
        Check(five.Active() && five.SecondaryMask() == 0x10 && five.GuestMask(3) == 0x08, "placement: five cores: one left for the rest");
        const PP::Plan gap{0x7C}; // cores 2 to 6: the guest takes the first four it may use, whatever their numbers
        Check(gap.Active() && gap.GuestMask(0) == 0x04 && gap.GuestMask(3) == 0x20 && gap.SecondaryMask() == 0x40, "placement: it follows the cores the console allows, not fixed numbers");
        const PP::Plan four{0x0F};
        Check(!four.Active() && four.For("CPUCore_0").role == Role::Free && four.GuestMask(0) == 0, "placement: four cores or fewer: nothing is pinned");
        const PP::Plan none{0};
        Check(!none.Active() && !PP::Plan{}.Active() && PP::Plan{}.For("GPU").mask == 0, "placement: an unknown mask (0) or no plan: nothing is pinned");
        Check(six.Describe().find("cores 0x3f") != std::string::npos && six.Describe().find("cores 0,1,2,3") != std::string::npos && six.Describe().find("on 4,5") != std::string::npos,
              "placement: the log line names the cores");
        Check(four.Describe().find("not used") != std::string::npos, "placement: the log line says when it is not used");
        bool disjoint = true;
        for (int i = 0; i < GuestCores; ++i) {
            for (int j = i + 1; j < GuestCores; ++j) {
                disjoint = disjoint && (six.GuestMask(i) & six.GuestMask(j)) == 0;
            }
            disjoint = disjoint && (six.GuestMask(i) & six.SecondaryMask()) == 0;
        }
        Check(disjoint, "placement: no core is given to two guest threads, and none to a guest thread and the rest");
    }

    {
        // 0.9.7 fix: the driver's arena inside one 4 GiB window (arena_place.h), against a model of the kernel's first-fit address space.
        namespace AP = Ps4ArenaPlace;
        constexpr std::uint64_t MiB = 1ull << 20;
        constexpr std::uint64_t GiB = 1ull << 30;
        // The kernel: gives the lowest address >= `floor` that is aligned and not inside a taken range, and takes it.
        struct Kernel {
            std::vector<std::pair<std::uint64_t, std::uint64_t>> taken; // start, length
            std::uint64_t floor = 0x200000000ull;
            std::uint64_t Reserve(std::uint64_t len) {
                std::uint64_t at = (floor + 2 * MiB - 1) / (2 * MiB) * (2 * MiB);
                for (bool moved = true; moved;) {
                    moved = false;
                    for (const auto& [start, size] : taken) {
                        if (at < start + size && start < at + len) {
                            at = (start + size + 2 * MiB - 1) / (2 * MiB) * (2 * MiB);
                            moved = true;
                        }
                    }
                }
                taken.emplace_back(at, len);
                return at;
            }
        };

        // The numbers of the Odyssey test (mesa.log): arena at 0x3d5c00000, 1536 MiB, RADV's window from 0x3d7c04000.
        Check(AP::PrivateBytes == 0x3d7c04000ull - 0x3d5c00000ull, "arena placement: the private slice is the 0x2004000 the driver keeps (mesa.log of the Odyssey test)");
        Check(AP::Usable32(0x3d5c00000ull, 1536 * MiB) == 0x400000000ull - 0x3d7c04000ull && AP::Usable32(0x3d5c00000ull, 1536 * MiB) / MiB == 643,
              "arena placement: the Odyssey arena had 643 MiB that could serve 32-bit allocations, not 1503");
        Check(!AP::Whole(0x3d5c00000ull, 1536 * MiB), "arena placement: ...so it is not whole");
        Check(AP::Whole(0x40d204000ull - AP::PrivateBytes, 1536 * MiB) && AP::Whole(0x400000000ull, 1536 * MiB) && AP::Whole(0x435c00000ull, 1536 * MiB),
              "arena placement: an arena from a window's start, or wholly inside one, is whole (the runs that passed)");
        Check(AP::Whole(0x400000000ull - 1536 * MiB, 1536 * MiB), "arena placement: an arena that ends exactly on a boundary is whole");
        Check(!AP::Whole(0x400000000ull - 1536 * MiB + 2 * MiB, 1536 * MiB), "arena placement: one 2 MiB step past the boundary is not");
        Check(AP::Whole(0x3ffffc000ull, 1536 * MiB), "arena placement: the driver's window is what counts (address32_hi comes from it): an arena that starts just below a boundary but whose window starts above it is whole");
        Check(AP::Usable32(0x1000, 0x1000) == 0 && !AP::Whole(0x1000, AP::PrivateBytes), "arena placement: an arena that cannot hold the private slice has nothing usable");

        // The Odyssey run: everything before the arena took the space up to 0x3d5c00000.
        {
            Kernel kernel;
            kernel.taken.emplace_back(0x200000000ull, 0x3d5c00000ull - 0x200000000ull);
            const AP::Outcome out = AP::Search([&](std::uint64_t len) { return kernel.Reserve(len); }, 1536 * MiB);
            Check(out.first == 0x3d5c00000ull && out.fillers == 1 && out.filler[0] == 0x3d5c00000ull && out.chosen == 0x435c00000ull && out.tries == 2 && !out.refused,
                  "arena placement: Odyssey layout - the straddling offer is kept as a filler and the next one, 0x435c00000, is chosen");
            Check(AP::Whole(out.chosen, 1536 * MiB) && out.chosen >> 32 == 4, "arena placement: ...and it lies in window 0x4");
            const std::string line = AP::Describe(out, 1536 * MiB, out.chosen);
            Check(line.find("0x3d5c00000") != std::string::npos && line.find("643 MiB of 1503 MiB") != std::string::npos && line.find("0x435c00000") != std::string::npos &&
                      line.find("inside one window") != std::string::npos,
                  "arena placement: the log line names the first offer, the 643 of 1503 MiB, the chosen address and the result");
            Check(AP::Describe(out, 1536 * MiB, 0x3d5c00000ull).find("STILL across a boundary") != std::string::npos,
                  "arena placement: a driver arena that still straddled is called so in the log");
        }
        // A good first offer: nothing to move, nothing kept.
        {
            Kernel kernel;
            kernel.taken.emplace_back(0x200000000ull, 0x40d204000ull - AP::PrivateBytes - 0x200000000ull);
            const AP::Outcome out = AP::Search([&](std::uint64_t len) { return kernel.Reserve(len); }, 1280 * MiB);
            Check(out.chosen == out.first && out.fillers == 0 && out.tries == 1, "arena placement: a first offer inside one window is taken as it is");
            Check(AP::Describe(out, 1280 * MiB, out.chosen).find("nothing to move") != std::string::npos, "arena placement: ...and the log says nothing was moved");
        }
        // Wherever the kernel starts, a few offers are enough for any arena the profiles give (a window repeats every 4 GiB).
        {
            bool all_placed = true;
            unsigned worst = 0;
            for (const std::uint64_t arena_mib : {1024ull, 1152ull, 1280ull, 1536ull, 2048ull}) {
                for (std::uint64_t start = 0x200000000ull; start < 0x600000000ull; start += 64 * MiB) {
                    Kernel kernel;
                    kernel.floor = start;
                    const AP::Outcome out = AP::Search([&](std::uint64_t len) { return kernel.Reserve(len); }, arena_mib * MiB);
                    all_placed = all_placed && out.chosen != 0 && AP::Whole(out.chosen, arena_mib * MiB);
                    worst = out.tries > worst ? out.tries : worst;
                }
            }
            Check(all_placed, "arena placement: for 1024 to 2048 MiB arenas and every start in 16 GiB, a window-whole range is found");
            Check(worst <= 4, "arena placement: ...in at most four reservations");
        }
        // The kernel refuses: the search stops, nothing is chosen, and the log says so.
        {
            unsigned asked = 0;
            const AP::Outcome out = AP::Search([&](std::uint64_t) -> std::uint64_t { return ++asked == 1 ? 0x3d5c00000ull : 0; }, 1536 * MiB);
            Check(out.refused && out.chosen == 0 && out.fillers == 1 && out.tries == 1 && asked == 2, "arena placement: a refused reservation ends the search with nothing chosen");
            Check(AP::Describe(out, 1536 * MiB, 0).find("left where the kernel puts it") != std::string::npos, "arena placement: ...and the log says the arena is left alone");
            const AP::Outcome none = AP::Search([](std::uint64_t) -> std::uint64_t { return 0; }, 1536 * MiB);
            Check(none.first == 0 && none.chosen == 0 && AP::Describe(none, 1536 * MiB, 0).find("refused to reserve") != std::string::npos,
                  "arena placement: no reservation at all is said as such");
        }
        // The search never asks more than MaxTries times, and never more than the caller allows.
        {
            unsigned asked = 0;
            AP::Search([&](std::uint64_t) -> std::uint64_t { return 0x3d5c00000ull + (asked++) * 0x1000000ull; }, 1536 * MiB);
            Check(asked == AP::MaxTries, "arena placement: at most MaxTries reservations (each only reserves address space)");
            unsigned asked_two = 0;
            AP::Search([&](std::uint64_t) -> std::uint64_t { return 0x3d5c00000ull + (asked_two++) * 0x1000000ull; }, 1536 * MiB, 2);
            Check(asked_two == 2, "arena placement: and no more than the caller allows");
        }
        // Which call is the driver's request for its arena.
        Check(AP::ArenaMiBFromEnv("1536") == 1536 && AP::ArenaMiBFromEnv(nullptr) == 0 && AP::ArenaMiBFromEnv("") == 0 && AP::ArenaMiBFromEnv("abc") == 0,
              "arena placement: ORBIS_ARENA_MIB is read the way the driver reads it");
        Check(AP::IsArenaRequest(1536, 1536 * MiB, 0, true), "arena placement: the arena request is the size the environment names, free address, no flags");
        Check(!AP::IsArenaRequest(1536, 512 * MiB, 0, true) && !AP::IsArenaRequest(1536, 128 * MiB, 0, true), "arena placement: a carve-out of the C heap is not the arena");
        Check(!AP::IsArenaRequest(1536, 1536 * MiB, 0x10, true) && !AP::IsArenaRequest(1536, 1536 * MiB, 0, false), "arena placement: a fixed or addressed mapping is not the arena");
        Check(!AP::IsArenaRequest(0, 1536 * MiB, 0, true) && !AP::IsArenaRequest(128, 128 * MiB, 0, true), "arena placement: no environment, or a bisect-size arena: not placed");
        (void)GiB;
    }
    std::printf(g_failures == 0 ? "memory guard test: PASSED\n" : "memory guard test: FAILED\n");
    return g_failures == 0 ? 0 : 1;
}
