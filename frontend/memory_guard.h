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
    int shader_preload = 256;    // pipelines' worth of shader cache compiled at start (see shader_policy.h)
    bool driver_cache = true;    // the driver's own pipeline cache file
    bool vram_in_arena = false;  // ORBIS_VRAM_GARLIC=0: every GPU image inside the arena (a hard cap; experimental)
};
/// `detected` is the tier measured on this console; a profile other than Auto replaces it.
Plan PlanFor(Profile profile, Tier detected);

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
