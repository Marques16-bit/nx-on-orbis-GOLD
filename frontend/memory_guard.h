// SPDX-License-Identifier: GPL-3.0-or-later
// eden-ps4 0.7.6: memory budget profiles per console and a memory watch. Pure logic (no console
// calls), so it is tested on a PC (tests/memory_guard_test.cpp); main.cpp feeds it the numbers.
//
// Background (docs/STATUS.md, "Memory budget"): the process gets about 4.5 GiB of direct memory and a
// race uses nearly all of it (guest RAM ~1.5 GiB, GPU driver arena 1.25 GiB, C heap 0.8-0.9 GiB, JIT,
// GARLIC images outside the arena). What runs out is the LARGEST FREE BLOCK, which is what
// sceKernelAvailableDirectMemorySize reports: the C heap takes 128 MiB carve-outs, and when no block
// that big is left the next allocation fails and the app dies.
//
// What this can and cannot do: it picks the memory-related defaults from what the console actually
// gives the process (so a console with less memory starts with a smaller GPU arena and shader
// preload), it shows a warning with the speed the free block is falling at, and it can pause the game
// before the crash. It does not free memory held by Eden (that needs changes inside Eden).

#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <string_view>

namespace Ps4Memory {

/// How much direct memory the process was given, in classes.
enum class Tier {
    Roomy,    // 4300 MiB or more: what the PS4 Pro measured (4608 MiB)
    Standard, // 3800 to 4299 MiB
    Tight,    // under 3800 MiB
};
Tier TierForBudget(std::uint64_t budget_mib);
const char* TierName(Tier tier, bool portuguese);

/// settings.txt: memory_profile=auto|roomy|standard|tight|strict
enum class Profile { Auto, Roomy, Standard, Tight, Strict };
bool ParseProfile(std::string_view text, Profile* out);

/// What a profile sets before settings.txt is read, so a line in settings.txt still wins.
struct Plan {
    Tier tier = Tier::Roomy;
    int arena_mib = 1280;        // ORBIS_ARENA_MIB: the GPU driver's arena
    int shader_preload = -1;     // pipelines' worth of shader cache compiled at start: -1 all (see shader_policy.h)
    bool driver_cache = true;    // the driver's own pipeline cache file
    bool vram_in_arena = false;  // ORBIS_VRAM_GARLIC=0: every GPU image inside the arena (a hard cap; experimental)
};
/// `detected` is the tier measured on this console; a profile other than Auto replaces it.
Plan PlanFor(Profile profile, Tier detected);

/// settings.txt: sysflex_mib=auto|off|256|512|768|1024|1536|2048 (128 and 384, the 0.9.0 values, are read as auto) - how much of the console's SYSTEM FLEXIBLE
/// memory pool the game's guest RAM may use (0.9.0, an idea from Eden4, which keeps its whole C heap there). That pool
/// is not part of the ~4.5 GiB of direct memory the rest of this file is about, so every MiB taken from it is a MiB of
/// direct memory kept free. 0.9.1: no longer experimental; "auto" (the default) is SystemFlexibleAutoCapMiB, still
/// limited by what the pool turns out to have. "off" is 0.
bool ParseSystemFlexibleCap(std::string_view text, std::uint64_t* out_mib);
/// What "auto" means: 1536 MiB. The 0.9.0 test on a PS4 Pro took 512 MiB (all of that budget) from the pool and the
/// system kept working; a firmware whose pool is smaller is handled by the probe and the reserve below.
constexpr std::uint64_t SystemFlexibleAutoCapMiB = 1536;
/// What a console has actually been seen to give (0.9.0, PS4 Pro). Safe mode (a start that died before drawing) goes
/// back to this when the cap is higher, instead of switching the pool off.
constexpr std::uint64_t SystemFlexibleProvenMiB = 512;
/// What is left to the system (its own libraries and thread objects come from the same pool): never taken.
constexpr std::uint64_t SystemFlexibleReserveMiB = 128;
/// The probe and the budget move in steps of this size.
constexpr std::uint64_t SystemFlexibleStepMiB = 32;
/// The budget actually used: the cap, but at most what the pool had free at start minus the reserve (`pool_mib`, as
/// found by a probe), in whole steps. 0 when the cap is 0 or the pool has less than the reserve plus one step to spare.
std::uint64_t SystemFlexibleBudgetMiB(std::uint64_t cap_mib, std::uint64_t pool_mib);
/// 0.9.1: the largest size in [reserve + step, max_mib], in steps, that `accepts` says the pool can map at once.
/// The first question is max_mib itself (a roomy pool answers yes at once, one probe); otherwise a binary search
/// between the smallest size and max_mib. 0 when even the smallest is refused. `accepts` is assumed monotonic (a
/// size the pool accepts, every smaller size it accepts too).
std::uint64_t LargestAcceptedMiB(std::uint64_t max_mib, const std::function<bool(std::uint64_t)>& accepts);

enum class Stage { Ok, Low, Critical };
enum class Action { Warn, Pause }; // settings.txt: memory_action=warn|pause
bool ParseAction(std::string_view text, Action* out);

struct Status {
    Stage stage = Stage::Ok;
    std::uint64_t free_mib = 0;       // the largest free block of direct memory
    double trend_mib_per_min = 0;     // negative: falling
    int eta_seconds = -1;             // until the critical level at the current speed, or -1 when not falling / not known
    bool entered_critical = false;    // true on the one update that crossed into Critical
};

/// One sample a second goes in; the stage (with hysteresis, so a block hovering at a limit does not
/// flap) and the speed of the fall come out.
class Watch {
public:
    static constexpr std::uint64_t DefaultLowMiB = 160;
    static constexpr std::uint64_t DefaultCriticalMiB = 64;
    explicit Watch(std::uint64_t low_mib = DefaultLowMiB, std::uint64_t critical_mib = DefaultCriticalMiB);
    Status Update(std::uint64_t free_mib, std::uint64_t now_ms);
    Stage stage() const { return stage_; }

private:
    struct Sample {
        std::uint64_t ms;
        std::uint64_t mib;
    };
    std::uint64_t low_, critical_;
    Stage stage_ = Stage::Ok;
    std::deque<Sample> samples_;
};

/// The line for the screen's corner notice, or "" at Stage::Ok.
std::string BannerText(const Status& status, bool portuguese);

} // namespace Ps4Memory
