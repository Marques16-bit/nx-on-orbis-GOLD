// SPDX-License-Identifier: GPL-3.0-or-later
// eden-ps4 0.7.6: memory profiles and the memory watch. See memory_guard.h.

#include <algorithm>
#include <cstdio>

#include "memory_guard.h"

namespace Ps4Memory {

namespace {
constexpr std::uint64_t HysteresisMiB = 32;       // how far above a limit the block must climb to leave a stage
constexpr std::uint64_t WindowMs = 30000;         // samples older than this are forgotten
constexpr std::uint64_t MinTrendMs = 10000;       // the speed of the fall needs at least this much history
constexpr double FallingMiBPerMin = -2.0;         // slower than this is noise, not a fall
} // Anonymous namespace

Tier TierForBudget(std::uint64_t budget_mib) {
    if (budget_mib >= 4300) {
        return Tier::Roomy;
    }
    return budget_mib >= 3800 ? Tier::Standard : Tier::Tight;
}

const char* TierName(Tier tier, bool portuguese) {
    switch (tier) {
    case Tier::Roomy:
        return portuguese ? "folgado (4,3 GiB ou mais, como o PS4 Pro medido)" : "roomy (4.3 GiB or more, like the PS4 Pro measured)";
    case Tier::Standard:
        return portuguese ? "padrão (3,8 a 4,3 GiB)" : "standard (3.8 to 4.3 GiB)";
    default:
        return portuguese ? "apertado (menos de 3,8 GiB)" : "tight (under 3.8 GiB)";
    }
}

bool ParseProfile(std::string_view text, Profile* out) {
    struct Entry {
        const char* name;
        Profile profile;
    };
    for (const Entry& e : {Entry{"auto", Profile::Auto}, Entry{"roomy", Profile::Roomy}, Entry{"standard", Profile::Standard},
                           Entry{"tight", Profile::Tight}, Entry{"strict", Profile::Strict}}) {
        if (text == e.name) {
            *out = e.profile;
            return true;
        }
    }
    return false;
}

bool ParseAction(std::string_view text, Action* out) {
    if (text == "warn") {
        *out = Action::Warn;
        return true;
    }
    if (text == "pause") {
        *out = Action::Pause;
        return true;
    }
    return false;
}

Plan PlanFor(Profile profile, Tier detected) {
    Tier tier = detected;
    switch (profile) {
    case Profile::Roomy:
        tier = Tier::Roomy;
        break;
    case Profile::Standard:
        tier = Tier::Standard;
        break;
    case Profile::Tight:
    case Profile::Strict:
        tier = Tier::Tight;
        break;
    case Profile::Auto:
        break;
    }
    Plan plan;
    plan.tier = tier;
    switch (tier) {
    case Tier::Roomy: // the values 0.7.3 and 0.7.4 always used, measured on a PS4 Pro
        plan.arena_mib = 1280;
        plan.shader_preload = 256;
        plan.driver_cache = true;
        break;
    case Tier::Standard:
        plan.arena_mib = 1152;
        plan.shader_preload = 128;
        plan.driver_cache = true;
        break;
    case Tier::Tight:
        plan.arena_mib = 1024;
        plan.shader_preload = 128;
        plan.driver_cache = false;
        break;
    }
    plan.vram_in_arena = profile == Profile::Strict;
    return plan;
}

Watch::Watch(std::uint64_t low_mib, std::uint64_t critical_mib) : low_(low_mib), critical_(std::min(critical_mib, low_mib)) {}

Status Watch::Update(std::uint64_t free_mib, std::uint64_t now_ms) {
    samples_.push_back({now_ms, free_mib});
    while (samples_.size() > 1 && now_ms - samples_.front().ms > WindowMs) {
        samples_.pop_front();
    }

    const Stage before = stage_;
    // Going down is immediate; going back up needs the hysteresis margin.
    Stage target = free_mib < critical_ ? Stage::Critical : free_mib < low_ ? Stage::Low : Stage::Ok;
    if (target < stage_) { // improving
        const std::uint64_t limit = stage_ == Stage::Critical ? critical_ : low_;
        if (free_mib < limit + HysteresisMiB) {
            target = stage_;
        } else if (stage_ == Stage::Critical && free_mib < low_) {
            target = Stage::Low;
        }
    }
    stage_ = target;

    Status status;
    status.stage = stage_;
    status.free_mib = free_mib;
    status.entered_critical = stage_ == Stage::Critical && before != Stage::Critical;
    const Sample& first = samples_.front();
    const std::uint64_t span = now_ms - first.ms;
    if (span >= MinTrendMs) {
        status.trend_mib_per_min = (double(free_mib) - double(first.mib)) * 60000.0 / double(span);
        if (status.trend_mib_per_min < FallingMiBPerMin && free_mib > critical_) {
            status.eta_seconds = int(double(free_mib - critical_) / -status.trend_mib_per_min * 60.0);
        }
    }
    return status;
}

std::string BannerText(const Status& status, bool portuguese) {
    char line[160];
    const unsigned long long free_mib = static_cast<unsigned long long>(status.free_mib);
    if (status.stage == Stage::Ok) {
        return {};
    }
    if (status.stage == Stage::Critical) {
        std::snprintf(line, sizeof(line), portuguese ? "MEMORIA CRITICA: %llu MiB livres. Salve o jogo agora" : "MEMORY CRITICAL: %llu MiB free. Save the game now",
                      free_mib);
        return line;
    }
    if (status.eta_seconds >= 0) {
        std::snprintf(line, sizeof(line),
                      portuguese ? "MEMORIA BAIXA: %llu MiB livres, caindo %.0f MiB/min (~%d s)" : "LOW MEMORY: %llu MiB free, falling %.0f MiB/min (~%d s)",
                      free_mib, -status.trend_mib_per_min, status.eta_seconds);
    } else {
        std::snprintf(line, sizeof(line), portuguese ? "MEMORIA BAIXA: %llu MiB livres" : "LOW MEMORY: %llu MiB free", free_mib);
    }
    return line;
}

} // namespace Ps4Memory
